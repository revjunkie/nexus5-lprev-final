/* Copyright (c) 2015, Raj Ibrahim <rajibrahim@rocketmail.com>. All rights reserved.
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

/* Revshift_hotplug
 * Based from Will Tisdale's (aka Thalamus) auto_hotplug.
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
#include <linux/delay.h>

#define SAMPLING_PERIODS 		12	
#define INDEX_MAX_VALUE		(SAMPLING_PERIODS - 1)

#define SHIFT_ALL				500
#define SHIFT_CPU1			270
#define SHIFT_CPU2			425
#define DOWN_SHIFT			100
#define MIN_CPU				1
#define MAX_CPU				4
#define TOUCHPLUG_DURATION		5000 /* 5 seconds */
#define SAMPLE_TIME	20

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

static struct delayed_work hotplug_decision_work;
static struct work_struct touchplug_boost_work;
static struct delayed_work touchplug_down;
static struct workqueue_struct *hotplug_decision_wq;
static struct workqueue_struct *touchplug_wq;

static unsigned int history[SAMPLING_PERIODS];
static unsigned int index;

static inline void hotplug_all(void)
{
	unsigned int cpu;
	for_each_possible_cpu(cpu) 
		if (!cpu_online(cpu)) 
			cpu_up(cpu);
	
}

static inline void hotplug_one(void)
{
	unsigned int cpu;
	
	for_each_possible_cpu(cpu) 
		if (cpu) {
			if (!cpu_online(cpu)) 
				cpu_up(cpu);		
				break;
	}
}

static inline void hotplug_offline(void)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) 
		if (num_online_cpus() > rev.min_cpu)
			if (cpu) {
				cpu_down(num_online_cpus() - 1); 
				break;
	}
}

static unsigned int  get_avg_running(void)
{
	unsigned int running, i, j, avg_running = 0;

	running = nr_running() * 100;
	history[index] = running;
		dprintk("index is: %d\n", index);
		dprintk("running is: %d\n", running);
	
	for (i = 0, j = index; i < rev.sampling_periods; i++, j--) {
		avg_running += history[j];
		if (unlikely(j == 0))
			j = INDEX_MAX_VALUE;
	}

	if (unlikely(index++ == INDEX_MAX_VALUE))
		index = 0;

	avg_running = avg_running / rev.sampling_periods;
	dprintk("average_running is: %d\n", avg_running);

	return avg_running;
}

static void  __cpuinit hotplug_decision_work_fn(struct work_struct *work)
{
	unsigned int online_cpus, available_cpus, disable_load, sampling_rate,  avg_running;
		
		avg_running = get_avg_running(); 
		online_cpus = num_online_cpus();
		available_cpus = rev.max_cpu;
		disable_load = rev.down_shift * online_cpus; 
		sampling_rate = msecs_to_jiffies(rev.sample_time) * online_cpus;
			dprintk("sampling_rate is: %d\n", jiffies_to_msecs(sampling_rate));

		if ((avg_running >= rev.shift_all) && (online_cpus < available_cpus)) {
			dprintk("auto_hotplug: Onlining all CPUs, avg running: %d\n", avg_running);
			hotplug_all();
				goto sched;
			}
		if ((avg_running >= rev.shift_cpu1) && (online_cpus < 2)) {
			if (touchplug) {
				goto sched;
			} else if (!touchplug) {
				 hotplug_one();
					goto sched;
			dprintk("auto_hotplug: Onlining CPU 1, avg running: %d\n", avg_running);
				}
			}
		 if ((avg_running >= rev.shift_cpu2) && (online_cpus < 3)) {
			dprintk("auto_hotplug: Onlining CPU 2, avg running: %d\n", avg_running);
			hotplug_one();
				goto sched;
			} 
		 if (avg_running <= disable_load) {
				dprintk("auto_hotplug: Offlining CPU, avg running: %d\n", avg_running);
			if (touchplug) {
				if (online_cpus == 2)
						schedule_delayed_work_on(0, &touchplug_down, msecs_to_jiffies(rev.touchplug_duration));
				else	
					hotplug_offline();
				goto sched;
				}
			if (!touchplug)
				hotplug_offline();	
					goto sched;
			}

	/*
	 * Reduce the sampling rate dynamically based on online cpus.
	 */
	sched:
	schedule_delayed_work_on(0, &hotplug_decision_work, sampling_rate);

}

static void __init touchplug_boost_work_fn(struct work_struct *work)
{
	if (num_online_cpus() == 1) 
 			cpu_up(1);	
	dprintk("touchplug detected\n");
}

static void  __init touchplug_down_fn(struct work_struct *work)
{
	if (num_online_cpus() == 2)
			cpu_down(1);
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

	if (new_val != rev.touchplug_duration && new_val >= 0 && new_val <= 30000)
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

int __init revshift_hotplug_init(void)
{
	int ret;

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
	INIT_DELAYED_WORK(&touchplug_down, touchplug_down_fn);
	INIT_WORK(&touchplug_boost_work, touchplug_boost_work_fn);

	schedule_delayed_work_on(0, &hotplug_decision_work, HZ * 10);
	return 0;
	
err:
	return ret;
}
late_initcall(revshift_hotplug_init);

