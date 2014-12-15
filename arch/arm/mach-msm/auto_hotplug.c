/* Copyright (c) 2012 - 2014, Will Tisdale <willtisdale@gmail.com>. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

/*
 * Generic auto hotplug driver for ARM SoCs. Targeted at current generation
 * SoCs with dual and quad core applications processors.
 * Automatically hotplugs online and offline CPUs based on system load.
 *
 * Not recommended for use with OMAP4460 due to the potential for lockups
 * whilst hotplugging.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/device.h>
#include <linux/miscdevice.h>

#define SAMPLING_PERIODS 		18	
#define INDEX_MAX_VALUE		(SAMPLING_PERIODS - 1)

#define SHIFT_ALL			500
#define SHIFT_CPU1			280
#define SHIFT_CPU2			450
#define DOWN_SHIFT			100
#define MIN_CPU			1
#define MAX_CPU			4
#define TOUCHPLUG_DURATION		5 /* seconds */
#define SAMPLE_TIME	20

/* Control flags */
unsigned char flags;
#define HOTPLUG_DISABLED	(1 << 0)
#define HOTPLUG_PAUSED	(1 << 1)

struct rev_tune
{
unsigned int shift_all;
unsigned int shift_cpu1;
unsigned int shift_cpu2;
unsigned int down_shift;
unsigned int min_cpu;
unsigned int max_cpu;
unsigned int touchplug_duration;
unsigned int sampling_periods;
unsigned int sample_time;
} rev = {
	.shift_all = SHIFT_ALL,
	.shift_cpu1 = SHIFT_CPU1,
	.shift_cpu2 = SHIFT_CPU2,
	.down_shift = DOWN_SHIFT,
	.min_cpu = MIN_CPU,
	.max_cpu = MAX_CPU,
	.touchplug_duration = TOUCHPLUG_DURATION,
	.sampling_periods = SAMPLING_PERIODS,
	.sample_time = SAMPLE_TIME,
};

static bool touchplug = true;
module_param(touchplug, bool, 0644);
static unsigned int debug = 0;
module_param(debug, uint, 0644);

#define dprintk(msg...)		\
do { 				\
	if (debug)		\
		pr_info(msg);	\
} while (0)

struct delayed_work hotplug_decision_work;
struct delayed_work hotplug_unpause_work;
struct work_struct hotplug_online_all_work;
struct work_struct hotplug_online_single_work;
struct work_struct touchplug_boost_work;
struct delayed_work hotplug_offline_work;

static struct workqueue_struct *hotplug_decision_wq;
static struct workqueue_struct *touchplug_wq;

static unsigned int history[SAMPLING_PERIODS];
static unsigned int index;

static void hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int running, disable_load, sampling_rate, avg_running = 0;
	unsigned int online_cpus, available_cpus, i, j;

	online_cpus = num_online_cpus();
	available_cpus = rev.max_cpu;
	disable_load = rev.down_shift * online_cpus; 
	/*
	 * Multiply nr_running() by 100 so we don't have to
	 * use fp division to get the average.
	 */
	running = nr_running() * 100;
	history[index] = running;
	dprintk("index is: %d\n", index);
	dprintk("running is: %d\n", running);

	/*
	 * Use a circular buffer to calculate the average load
	 * over the sampling periods.
	 * This will absorb load spikes of short duration where
	 * we don't want additional cores to be onlined because
	 * the cpufreq driver should take care of those load spikes.
	 */
	for (i = 0, j = index; i < rev.sampling_periods; i++, j--) {
		avg_running += history[j];
		if (unlikely(j == 0))
			j = INDEX_MAX_VALUE;
	}

	/*
	 * If we are at the end of the buffer, return to the beginning.
	 */
	if (unlikely(index++ == INDEX_MAX_VALUE))
		index = 0;

	avg_running = avg_running / rev.sampling_periods;
	dprintk("average_running is: %d\n", avg_running);

	if (likely(!(flags & HOTPLUG_DISABLED))) {
		if (unlikely((avg_running >= rev.shift_all) && (online_cpus < available_cpus))) {
			dprintk("auto_hotplug: Onlining all CPUs, avg running: %d\n", avg_running);
			/*
			 * Flush any delayed offlining work from the workqueue.
			 * No point in having expensive unnecessary hotplug transitions.
			 * We still online after flushing, because load is high enough to
			 * warrant it.
			 * We set the paused flag so the sampling can continue but no more
			 * hotplug events will occur.
			 */
			flags |= HOTPLUG_PAUSED;
			if (delayed_work_pending(&hotplug_offline_work))
				cancel_delayed_work(&hotplug_offline_work);
			schedule_work(&hotplug_online_all_work);
			return;
		} else if (flags & HOTPLUG_PAUSED) {
			schedule_delayed_work_on(0, &hotplug_decision_work, msecs_to_jiffies(rev.sample_time)); 
			return;
			/* Seperate the threshold from cpu1 and cpu2 to have greater control, 
			*  since we already added a generic input boost function to online cpu1.
			* We still use this threshold when the device is idling, to prevent unnecessary
			* onlining of cpu1.
			*/
		} else if ((avg_running >= rev.shift_cpu1) && (online_cpus < 2)) {
			if (touchplug) {
				cancel_work_sync(&hotplug_online_single_work);				
				schedule_delayed_work_on(0, &hotplug_decision_work, msecs_to_jiffies(rev.sample_time));
			return;
			} else if (!touchplug) {
				 schedule_work(&hotplug_online_single_work);
			dprintk("auto_hotplug: Onlining CPU 1, avg running: %d\n", avg_running);
			return;
			}
		} else if ((avg_running >= rev.shift_cpu2) && (online_cpus < 3)) {
			dprintk("auto_hotplug: Onlining CPU 2, avg running: %d\n", avg_running);
			schedule_work(&hotplug_online_single_work);
			return;
		} else if (avg_running <= disable_load) {
			/* Only queue a cpu_down() if there isn't one already pending */
			if (!(delayed_work_pending(&hotplug_offline_work))) {
				dprintk("auto_hotplug: Offlining CPU, avg running: %d\n", avg_running);
			if (touchplug) {
				if (online_cpus == 2) 
				schedule_delayed_work_on(0, &hotplug_offline_work, HZ * rev.touchplug_duration);
		 		else 		
				schedule_delayed_work_on(0, &hotplug_offline_work, HZ);
				return;
			} else if (!touchplug) {
				schedule_delayed_work_on(0, &hotplug_offline_work, HZ);
			return;
				}
			}
		}
	}

	/*
	 * Reduce the sampling rate dynamically based on online cpus.
	 */
	sampling_rate = msecs_to_jiffies(rev.sample_time) * online_cpus;

	dprintk("sampling_rate is: %d\n", jiffies_to_msecs(sampling_rate));

	schedule_delayed_work_on(0, &hotplug_decision_work, sampling_rate);

}

static void __init hotplug_online_all_work_fn(struct work_struct *work)
{
	unsigned int cpu;
	for_each_possible_cpu(cpu) {
		if (likely(!cpu_online(cpu))) 
			cpu_up(cpu);
	}
	/*
	 * Pause for 1 second before even considering offlining a CPU
	 */
	schedule_delayed_work(&hotplug_unpause_work, HZ);
	schedule_delayed_work_on(0, &hotplug_decision_work, msecs_to_jiffies(rev.sample_time));
}

static void __init hotplug_online_single_work_fn(struct work_struct *work)
{
	unsigned int cpu;
	
	for_each_possible_cpu(cpu) {
		if (cpu) {
			if (!cpu_online(cpu)) {
				cpu_up(cpu);
				break;	
			}
		}
	}
	schedule_delayed_work_on(0, &hotplug_decision_work, msecs_to_jiffies(rev.sample_time));
}

static void __init touchplug_boost_work_fn(struct work_struct *work)
{
	if (num_online_cpus() == 1) 
 			cpu_up(1);	
	
	schedule_delayed_work_on(0, &hotplug_decision_work, 0);
}
static void hotplug_offline_work_fn(struct work_struct *work)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		if (num_online_cpus() > rev.min_cpu)
			if (cpu) {
				cpu_down(num_online_cpus() - 1);
				break;
		}
	}
	schedule_delayed_work_on(0, &hotplug_decision_work, msecs_to_jiffies(rev.sample_time));
}

static void hotplug_unpause_work_fn(struct work_struct *work)
{
	dprintk("auto_hotplug: Clearing pause flag\n");
	flags &= ~HOTPLUG_PAUSED;
}

void hotplug_disable(bool flag)
{
	if (flags & HOTPLUG_DISABLED && !flag) {
		flags &= ~HOTPLUG_DISABLED;
		flags &= ~HOTPLUG_PAUSED;

		dprintk("auto_hotplug: Clearing disable flag\n");
		
		schedule_delayed_work_on(0, &hotplug_decision_work, 0);
	} else if (flag && (!(flags & HOTPLUG_DISABLED))) {
		flags |= HOTPLUG_DISABLED;
		dprintk("auto_hotplug: Setting disable flag\n");
		cancel_delayed_work_sync(&hotplug_offline_work);
		cancel_delayed_work_sync(&hotplug_decision_work);
		cancel_delayed_work_sync(&hotplug_unpause_work);
	}
}

/**************SYSFS*******************/

static ssize_t shift_cpu1_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.shift_cpu1);
}

static ssize_t shift_cpu1_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != rev.shift_cpu1 && new_val >= 0 && new_val <= 500)
	{
		rev.shift_cpu1 = new_val;
	}

	return size;
}

static ssize_t shift_cpu2_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.shift_cpu2);
}

static ssize_t shift_cpu2_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != rev.shift_cpu2 && new_val >= 0 && new_val <= 500)
	{
		rev.shift_cpu2 = new_val;
	}

	return size;
}

static ssize_t shift_all_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.shift_all);
}

static ssize_t shift_all_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != rev.shift_all && new_val >= 0 && new_val <= 600)
	{
		rev.shift_all = new_val;
	}

	return size;
}

static ssize_t down_shift_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.down_shift);
}

static ssize_t down_shift_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != rev.down_shift && new_val >= 0 && new_val <= 200)
	{
		rev.down_shift = new_val;
	}

	return size;
}

static ssize_t min_cpu_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.min_cpu);
}

static ssize_t min_cpu_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != rev.min_cpu && new_val >= 1 && new_val <= 4)
	{
		rev.min_cpu = new_val;
	}

	return size;
}

static ssize_t max_cpu_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.max_cpu);
}

static ssize_t max_cpu_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != rev.max_cpu && new_val >= 1 && new_val <= 4)
	{
		rev.max_cpu = new_val;
	}

	return size;
}

static ssize_t touchplug_duration_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.touchplug_duration);
}

static ssize_t touchplug_duration_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int new_val;

	sscanf(buf, "%u", &new_val);

	if (new_val != rev.touchplug_duration && new_val >= 0 && new_val <= 100)
	{
		rev.touchplug_duration = new_val;
	}

	return size;
}

static ssize_t sampling_periods_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.sampling_periods);
}

static ssize_t sampling_periods_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	if (val != rev.sampling_periods && val >= 1 && val <= 500)
	{
		rev.sampling_periods = val;
	}

	return size;
}

static ssize_t sample_time_show(struct device * dev, struct device_attribute * attr, char * buf)
{
	return sprintf(buf, "%d\n", rev.sample_time);
}

static ssize_t sample_time_store(struct device * dev, struct device_attribute * attr, const char * buf, size_t size)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	if (val != rev.sample_time && val >= 1 && val <= 500)
	{
		rev.sample_time = val;
	}

	return size;
}
static DEVICE_ATTR(shift_cpu1, 0644, shift_cpu1_show, shift_cpu1_store);
static DEVICE_ATTR(shift_cpu2, 0644, shift_cpu2_show, shift_cpu2_store);
static DEVICE_ATTR(shift_all, 0644, shift_all_show, shift_all_store);
static DEVICE_ATTR(down_shift, 0644, down_shift_show, down_shift_store);
static DEVICE_ATTR(min_cpu, 0644, min_cpu_show, min_cpu_store);
static DEVICE_ATTR(max_cpu, 0644, max_cpu_show, max_cpu_store);
static DEVICE_ATTR(touchplug_duration, 0644, touchplug_duration_show, touchplug_duration_store);
static DEVICE_ATTR(sampling_periods, 0644, sampling_periods_show, sampling_periods_store);
static DEVICE_ATTR(sample_time, 0644, sample_time_show, sample_time_store);

static struct attribute *revshift_hotplug_attributes[] = 
    {
	&dev_attr_shift_cpu1.attr,
	&dev_attr_shift_cpu2.attr,
	&dev_attr_shift_all.attr,
	&dev_attr_down_shift.attr,
	&dev_attr_min_cpu.attr,
	&dev_attr_max_cpu.attr,
	&dev_attr_touchplug_duration.attr,
	&dev_attr_sampling_periods.attr,
	&dev_attr_sample_time.attr,
	NULL
    };

static struct attribute_group revshift_hotplug_group = 
    {
	.attrs  = revshift_hotplug_attributes,
    };

static struct miscdevice revshift_hotplug_device = 
    {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "revshift_hotplug",
    };


static void touchplug_input_event(struct input_handle *handle,
 		unsigned int type, unsigned int code, int value)
{
 	if (touchplug) {
	dprintk("touchplug detected\n");	
	queue_work(touchplug_wq, &touchplug_boost_work);
	}
}
 
static int touchplug_input_connect(struct input_handler *handler,
 		struct input_dev *dev, const struct input_device_id *id)
{
 	struct input_handle *handle;
 	int error;
 
 	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
 	if (!handle)
 		return -ENOMEM;
 
 	handle->dev = dev;
 	handle->handler = handler;
 	handle->name = "touchplug_input_handler";

 
 	error = input_register_handle(handle);
 	if (error)
 		goto err2;
 
 	error = input_open_device(handle);
 	if (error)
 		goto err1;
 	dprintk("%s found and connected!\n", dev->name);
 	return 0;
 err1:
 	input_unregister_handle(handle);
 err2:
 	kfree(handle);
 	return error;
}
 
static void touchplug_input_disconnect(struct input_handle *handle)
{
 	input_close_device(handle);
 	input_unregister_handle(handle);
 	kfree(handle);
}

static const struct input_device_id touchplug_ids[] = {
 	{
 		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
 			 INPUT_DEVICE_ID_MATCH_ABSBIT,
 		.evbit = { BIT_MASK(EV_ABS) },
 		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
 			    BIT_MASK(ABS_MT_POSITION_X) |
 			    BIT_MASK(ABS_MT_POSITION_Y) },
 	}, /* multi-touch touchscreen */
 	{
 		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
 			 INPUT_DEVICE_ID_MATCH_ABSBIT,
 		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
 		.absbit = { [BIT_WORD(ABS_X)] =
 			    BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
 	}, /* touchpad */
 	{ },
};
 
static struct input_handler touchplug_input_handler = {
 	.event          = touchplug_input_event,
 	.connect        = touchplug_input_connect,
 	.disconnect     = touchplug_input_disconnect,
 	.name           = "touchplug_input_handler",
 	.id_table       = touchplug_ids,
};

int __init auto_hotplug_init(void)
{
	int ret;
		
	pr_info("auto_hotplug: v0.220 by _thalamus\n");
	dprintk("auto_hotplug: %d CPUs detected\n", num_possible_cpus());

	ret = input_register_handler(&touchplug_input_handler);
	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}
	ret = misc_register(&revshift_hotplug_device);
	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}
	ret = sysfs_create_group(&revshift_hotplug_device.this_device->kobj,
			&revshift_hotplug_group);

	if (ret)
	{
		ret = -EINVAL;
		goto err;
	}
	hotplug_decision_wq = alloc_workqueue("hotplug_decision_work",
				WQ_HIGHPRI | WQ_UNBOUND, 0);
	touchplug_wq = alloc_workqueue("touchplug", WQ_HIGHPRI, 0);	

	INIT_DELAYED_WORK(&hotplug_decision_work, hotplug_decision_work_fn);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_unpause_work, hotplug_unpause_work_fn);
	INIT_WORK(&hotplug_online_all_work, hotplug_online_all_work_fn);
	INIT_WORK(&hotplug_online_single_work, hotplug_online_single_work_fn);
	INIT_WORK(&touchplug_boost_work, touchplug_boost_work_fn);
	INIT_DELAYED_WORK_DEFERRABLE(&hotplug_offline_work, hotplug_offline_work_fn);

	/*
	 * Give the system time to boot before fiddling with hotplugging.
	 */
	flags |= HOTPLUG_PAUSED;
	schedule_delayed_work_on(0, &hotplug_decision_work, HZ * 10);
	schedule_delayed_work(&hotplug_unpause_work, HZ * 20);
	return 0;
	
err:
	return ret;
}
late_initcall(auto_hotplug_init);

