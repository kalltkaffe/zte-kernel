/* drivers/input/keyboard/synaptics_i2c_rmi.c
 *
 * Copyright (C) 2007 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
/* ========================================================================================
when         who        what, where, why                         comment tag
--------     ----       -------------------------------------    --------------------------
2011-03-17   zfj        rotate x and y position for P725A        ZTE_TS_ZFJ_20110317
2011-03-02   zfj        use create_singlethread_workqueue instead    ZTE_TS_ZFJ_20110302 
2011-03-15   zfj        modify touchscreen enable gpio           ZTE_TS_ZFJ_20110315
2010-12-28	 zfj        add synaptics 3k touchscreen macro       ZTE_TS_ZFJ_20101228
2010-10-14   liwei      change filter threshold.                 ZTE_TOUCH_LIWEI_20101014
2010-09-01   xuke       add filter.                              ZTE_XUKE_TOUCH_20100901
2010-09-01	xiayc		remove POLLING mode.					ZTE-XIAYC_20100901,ZTE_WLY_CRDB00533288
2010-06-22   wly         config 8 bit adress                      ZTE_WLY_CRDB00512790
2010-06-10   wly         touchscreen firmware information         ZTE_WLY_CRDB00509514
2010-05-24   wly            change pressure value                     ZTE_PRESS_WLY_0524
2010-05-20   zt  	    modified the y axis for P727A1					ZTE_TS_ZT_20100520_001
2010-05-13	 zt		    modified the ts configuration for R750		ZTE_TS_ZT_20100513_002
2010-05-18   wly         config set bit                                 ZTE_SET_BIT_WLY_0518
2010-3-18    wly        add gesture and resume timer             	  ZTE_WLY_RESUME_001
2010-2-27    wly        add for limo                                  ZTE_WLY_LOCK_001
2010-02-04	 chj		protect two timer booming at the same time    ZTE_TOUCH_CHJ_010
2010-02-03	 chj		moving polling process into the interrpt      ZTE_TOUCH_CHJ_009
2010-01-19   wly        add proc interface                            ZTE_TOUCH_WLY_008
2010-01-19   wly        down cpu use                                  ZTE_TOUCH_WLY_007
2010-01-06   wly        add synaptics gesture                         ZTE_TOUCH_WLY_006
2009-12-19   wly        change synaptics driver                       ZTE_TOUCH_WLY_005
========================================================================================*/
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/proc_fs.h>
#include <linux/platform_device.h>
#include <linux/synaptics_i2c_rmi.h>
#if 1 //wly
#include <mach/gpio.h>
#endif


#if defined(CONFIG_MACH_BLADE)//P729B touchscreen enable
#define GPIO_TOUCH_EN_OUT  31
#elif defined(CONFIG_MACH_R750)//R750 touchscreen enable
#define GPIO_TOUCH_EN_OUT  33
#elif defined(CONFIG_MACH_TURIES)
#define GPIO_TOUCH_EN_OUT 89  	
#elif defined(CONFIG_MACH_MOONCAKE)
#define GPIO_TOUCH_EN_OUT  30
#else//other projects
#define GPIO_TOUCH_EN_OUT  31
#endif



#if defined(CONFIG_MACH_R750)
//#define TOUCHSCREEN_DUPLICATED_FILTER
#define LCD_MAX_X   320
#define LCD_MAX_Y   480
#elif  defined(CONFIG_MACH_JOE)
//#define TOUCHSCREEN_DUPLICATED_FILTER
#define LCD_MAX_X   240
#define LCD_MAX_Y   400
#elif  defined(CONFIG_MACH_BLADE)
#define TOUCHSCREEN_DUPLICATED_FILTER
#define LCD_MAX_X   480
#define LCD_MAX_Y   800
#endif


#define ABS_SINGLE_TAP	0x21	/* Major axis of touching ellipse */
#define ABS_TAP_HOLD	0x22	/* Minor axis (omit if circular) */
#define ABS_DOUBLE_TAP	0x23	/* Major axis of approaching ellipse */
#define ABS_EARLY_TAP	0x24	/* Minor axis (omit if circular) */
#define ABS_FLICK	0x25	/* Ellipse orientation */
#define ABS_PRESS	0x26	/* Major axis of touching ellipse */
#define ABS_PINCH 	0x27	/* Minor axis (omit if circular) */
#define sigle_tap  (1 << 0)
#define tap_hold   (1 << 1)
#define double_tap (1 << 2)
#define early_tap  (1 << 3)
#define flick      (1 << 4)
#define press      (1 << 5)
#define pinch      (1 << 6)


unsigned long polling_time = 12500000;


static struct workqueue_struct *synaptics_wq;
static struct i2c_driver synaptics_ts_driver;
//#define swap(x, y) do { typeof(x) z = x; x = y; y = z; } while (0)

#define POLL_IN_INT   
#if defined (POLL_IN_INT)
#undef POLL_IN_INT   
#endif


#if defined(CONFIG_MACH_TURIES)
static int p725a_max_y = 0;  
#endif

struct synaptics_ts_data
{
	uint16_t addr;
	struct i2c_client *client;
	struct input_dev *input_dev;
	int use_irq;
	struct hrtimer timer;

	//struct hrtimer resume_timer;  

	struct work_struct  work;
	uint16_t max[2];
	struct early_suspend early_suspend;
	uint32_t dup_threshold;    
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void synaptics_ts_early_suspend(struct early_suspend *h);
static void synaptics_ts_late_resume(struct early_suspend *h);
#endif

#if defined(CONFIG_TOUCHSCREEN_VIRTUAL_KEYS)
#define virtualkeys virtualkeys.synaptics-rmi-touchscreen
#if defined(CONFIG_MACH_MOONCAKE)
static const char ts_keys_size[] = "0x01:102:42:335:10:10:0x01:158:204:335:10:10";
#else
static const char ts_keys_size[] = "0x01:102:51:503:102:1007:0x01:139:158:503:102:1007:0x01:158:266:503:102:1007";
#endif
static ssize_t virtualkeys_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	sprintf(buf,"%s\n",ts_keys_size);
	printk("wly:%s\n",__FUNCTION__);
    return strlen(ts_keys_size)+2;
}
static DEVICE_ATTR(virtualkeys, 0444, virtualkeys_show, NULL);
extern struct kobject *android_touch_kobj;
static struct kobject * virtual_key_kobj;
static int ts_key_report_init(void)
{
	int ret;
	virtual_key_kobj = kobject_get(android_touch_kobj);
	if (virtual_key_kobj == NULL) {
		virtual_key_kobj = kobject_create_and_add("board_properties", NULL);
		if (virtual_key_kobj == NULL) {
			printk(KERN_ERR "%s: subsystem_register failed\n", __func__);
			ret = -ENOMEM;
			return ret;
}
}
 
	ret = sysfs_create_file(virtual_key_kobj, &dev_attr_virtualkeys.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
}
 
	return 0;
}
static void ts_key_report_deinit(void)
{
	sysfs_remove_file(virtual_key_kobj, &dev_attr_virtualkeys.attr);
	kobject_del(virtual_key_kobj);
}
#endif
#if 1 
static int synaptics_i2c_read(struct i2c_client *client, int reg, u8 * buf, int count)
{
    int rc;
    int ret = 0;

    buf[0] = reg;
    rc = i2c_master_send(client, buf, 1);
    if (rc != 1)
    {
        dev_err(&client->dev, "synaptics_i2c_read FAILED: read of register %d\n", reg);
        ret = -1;
        goto tp_i2c_rd_exit;
    }
    rc = i2c_master_recv(client, buf, count);
    if (rc != count)
    {
        dev_err(&client->dev, "synaptics_i2c_read FAILED: read %d bytes from reg %d\n", count, reg);
        ret = -1;
    }

  tp_i2c_rd_exit:
    return ret;
}
static int synaptics_i2c_write(struct i2c_client *client, int reg, u8 data)
{
    u8 buf[2];
    int rc;
    int ret = 0;

    buf[0] = reg;
    buf[1] = data;
    rc = i2c_master_send(client, buf, 2);
    if (rc != 2)
    {
        dev_err(&client->dev, "synaptics_i2c_write FAILED: writing to reg %d\n", reg);
        ret = -1;
    }
    return ret;
}
#else

static int synaptics_i2c_read(struct i2c_client *client, int reg, u8 * buf, int count)
{
    int rc;
    int ret = 0;

    buf[0] = 0xff;
	buf[1] = reg >> 8;
    rc = i2c_master_send(client, buf, 2);
    if (rc != 2)
{
        dev_err(&client->dev, "synaptics_i2c_read FAILED: failed of page select %d\n", rc);
        ret = -1;
        goto tp_i2c_rd_exit;
    }
	buf[0] = 0xff & reg;
	rc = i2c_master_send(client, buf, 1);
    if (rc != 1)
    {
        dev_err(&client->dev, "synaptics_i2c_read FAILED: read of register %d\n", reg);
        ret = -1;
        goto tp_i2c_rd_exit;
    }
    rc = i2c_master_recv(client, buf, count);
    if (rc != count)
    {
        dev_err(&client->dev, "synaptics_i2c_read FAILED: read %d bytes from reg %d\n", count, reg);
        ret = -1;
    }

  tp_i2c_rd_exit:
    return ret;
	}
static int synaptics_i2c_write(struct i2c_client *client, int reg, u8 data)
{
    u8 buf[2];
    int rc;
    int ret = 0;

    buf[0] = 0xff;
    buf[1] = reg >> 8;
    rc = i2c_master_send(client, buf, 2);
    if (rc != 2)
    {
        dev_err(&client->dev, "synaptics_i2c_write FAILED: writing to reg %d\n", reg);
        ret = -1;
    }
	buf[0] = 0xff & reg;
    buf[1] = data;
    rc = i2c_master_send(client, buf, 2);
    if (rc != 2)
    {
        dev_err(&client->dev, "synaptics_i2c_write FAILED: writing to reg %d\n", reg);
        ret = -1;
    }
	return ret;
}
#endif  //


/*static int proc_read_val(char *page, char **start,
           off_t off, int count, int *eof, void *data)
{
        int len;
        len = sprintf(page, "%lu\n", polling_time);
        return len;
}*/
static int
proc_read_val(char *page, char **start, off_t off, int count, int *eof,
	  void *data)
{
	int len = 0;
	len += sprintf(page + len, "%s\n", "touchscreen module");
	len += sprintf(page + len, "name     : %s\n", "synaptics");
	#if defined(CONFIG_MACH_R750)
	len += sprintf(page + len, "i2c address  : %x\n", 0x23);
	#else
	len += sprintf(page + len, "i2c address  : 0x%x\n", 0x22);
	#endif
	#if defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
	len += sprintf(page + len, "IC type    : %s\n", "3000 series");
	#else
	len += sprintf(page + len, "IC type    : %s\n", "2000 series");
	#endif
	#if defined(CONFIG_MACH_R750)
	len += sprintf(page + len, "firmware version    : %s\n", "TM1551");
	#elif  defined(CONFIG_MACH_JOE)
	len += sprintf(page + len, "firmware version    : %s\n", "TM1419-001");
	#elif  defined(CONFIG_MACH_BLADE)
	len += sprintf(page + len, "firmware version    : %s\n", "TM1541");
	#endif
	len += sprintf(page + len, "module : %s\n", "synaptics + TPK");
	if (off + count >= len)
		*eof = 1;
	if (len < off)
		return 0;
	*start = page + off;
	return ((count < len - off) ? count : len - off);
}


static int proc_write_val(struct file *file, const char *buffer,
           unsigned long count, void *data)
{
		unsigned long val;
		sscanf(buffer, "%lu", &val);
		if (val >= 0) {
			polling_time= val;
			return count;
		}
		return -EINVAL;
}




#ifdef TOUCHSCREEN_DUPLICATED_FILTER
static int duplicated_filter(struct synaptics_ts_data *ts, int x,int y,int x2,int y2,
						const int finger2, const int z)
{
	int drift_x[2];
	int drift_y[2];
	static int ref_x[2], ref_y[2];
	uint8_t discard[2] = {0, 0};

	drift_x[0] = abs(ref_x[0] - x);
	drift_y[0] = abs(ref_y[0] - y);
	if (finger2) {
		drift_x[1] = abs(ref_x[1] - x2);
		drift_y[1] = abs(ref_y[1] - y2);
	}
	/* printk("ref_x :%d, ref_y: %d, x: %d, y: %d\n", ref_x, ref_y, pos[0][0], pos[0][1]); */
	if (drift_x[0] < ts->dup_threshold && drift_y[0] < ts->dup_threshold && z != 0) {
		/* printk("ref_x :%d, ref_y: %d, x: %d, y: %d\n", ref_x[0], ref_y[0], pos[0][0], pos[0][1]); */
		discard[0] = 1;
	}
	if (!finger2 || (drift_x[1] < ts->dup_threshold && drift_y[1] < ts->dup_threshold)) {
		discard[1] = 1;
	}
	if (discard[0] && discard[1]) {
		/* if finger 0 and finger 1's movement < threshold , discard it. */
		return 1;
	}
	ref_x[0] = x;
	ref_y[0] = y;
	if (finger2) {
		ref_x[1] = x2;
		ref_y[1] = y2;
	}
	if (z == 0) {
		ref_x[0] = ref_y[0] = 0;
		ref_x[1] = ref_y[1] = 0;
	}

	return 0;
}
#endif /* TOUCHSCREEN_DUPLICATED_FILTER */

static void synaptics_ts_work_func(struct work_struct *work)
{
  
	int ret, x, y, z, finger, w, x2, y2,w2,z2,finger2,pressure,pressure2;
  
	__s8  gesture, flick_y, flick_x, direction = 0; 
  #if defined(CONFIG_TOUCHSCREEN_VIRTUAL_KEYS)
  #if defined(CONFIG_MACH_MOONCAKE)
  	static int x_temp,y_temp;
  #endif
  #endif
	uint8_t buf[16];
	struct synaptics_ts_data *ts = container_of(work, struct synaptics_ts_data, work);
	finger=0;//initializing the status
	#if defined(CONFIG_MACH_SKATE)
	ret = synaptics_i2c_read(ts->client, 0x14, buf, 16);
	#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
	ret = synaptics_i2c_read(ts->client, 0x14, buf, 16);  
	#else
	ret = synaptics_i2c_read(ts->client, 0x14, buf, 16);
	#endif
	if (ret < 0){
   	printk(KERN_ERR "synaptics_ts_work_func: synaptics_i2c_write failed, go to poweroff.\n");
    gpio_direction_output(GPIO_TOUCH_EN_OUT, 0);
    msleep(200);
    gpio_direction_output(GPIO_TOUCH_EN_OUT, 1);
    msleep(200);
  }
  else
  {
			/*printk(KERN_WARNING "synaptics_ts_work_func:"
			"%x %x %x %x %x %x %x %x %x"
					       " %x %x %x %x %x %x, ret %d\n",
					       buf[0], buf[1], buf[2], buf[3],
					       buf[4], buf[5], buf[6], buf[7],
	        buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], ret);*/
			#if defined(CONFIG_MACH_SKATE)||defined(CONFIG_MACH_MOONCAKE)
			x = (uint16_t) buf[3] << 4| (buf[5] & 0x0f) ; 
			y = (uint16_t) buf[4] << 4| ((buf[5] & 0xf0) >> 4); 
			pressure = buf[7];
			w = buf[6] >> 4;
			z = buf[6]&0x0f;
			finger = buf[1] & 0x3;
	
			x2 = (uint16_t) buf[8] << 4| (buf[10] & 0x0f) ;  
			y2 = (uint16_t) buf[9] << 4| ((buf[10] & 0xf0) >> 4); 
			pressure2 = buf[12]; 
			w2 = buf[11] >> 4; 
			z2 = buf[11] & 0x0f;
	        
			finger2 = buf[1] & 0xc; 
			#else
			x = (uint16_t) buf[2] << 4| (buf[4] & 0x0f) ; 
			y = (uint16_t) buf[3] << 4| ((buf[4] & 0xf0) >> 4); 
			pressure = buf[6];
			w = buf[5] >> 4;
			z = buf[5]&0x0f;
			finger = buf[1] & 0x3;
	
			x2 = (uint16_t) buf[7] << 4| (buf[9] & 0x0f) ;  
			y2 = (uint16_t) buf[8] << 4| ((buf[9] & 0xf0) >> 4); 
			pressure2 = buf[11]; 
			w2 = buf[10] >> 4; 
			z2 = buf[10] & 0x0f;
			finger2 = buf[1] & 0xc; 
			gesture = buf[12];
			//printk("wly: finger=%d, finger2=%d, buf[1]=%d\n", finger, finger2, buf[1]);
			#endif
			
			#ifdef CONFIG_MACH_JOE
			y = 2787 - y;
			y2 = 2787 - y2;
			#endif
	
			//pr_info("%s, x=%d, y=%d, x2=%d, y2=%d\n", __func__, x, y ,x2, y2);
			flick_x = buf[14];
			flick_y = buf[15];
			//printk("wly: gesture=%d,flick_x=%d,flick_y=%d\n",gesture,flick_x,flick_y);
			if((16==gesture)||(flick_x)||(flick_y))
			{
				if ((flick_x >0 )&& (abs(flick_x) > abs(flick_y))) 
				direction = 1;//�һ�
				else if((flick_x <0 )&& (abs(flick_x) > abs(flick_y)))  
				direction = 2;//��
				else if ((flick_y >0 )&& (abs(flick_x) < abs(flick_y))) 
				direction = 3;//�ϻ�
	
				else if ((flick_y <0 )&& (abs(flick_x) < abs(flick_y))) 
				direction = 4;//�»�

			}
			/*fick_x>0,means move apart, flick_y<0,means close together, the value means velocity*/
			
			#if defined(CONFIG_TOUCHSCREEN_VIRTUAL_KEYS)
			#if defined(CONFIG_MACH_MOONCAKE)
			if(buf[0]&0x04)
			{
				if(buf[1]==0x01)
				{
					x= 320;
					y= 2500;
					pressure = 50;
					x_temp=x;
					y_temp=y;
					
					//input_report_key(ts->input_dev, BTN_TOUCH, 1);
				}else if(buf[1]==0x02)
				{
					x= 1540;
					y= 2500;
					pressure = 50;
					x_temp=x;
					y_temp=y;
					//input_report_key(ts->input_dev, BTN_TOUCH, 1);

				}else
				{
					x=x_temp;
					y=y_temp;
					//input_report_key(ts->input_dev, BTN_TOUCH, 0);
				}
			}
			#endif
			#endif
			


#if defined(CONFIG_MACH_TURIES)
	y = p725a_max_y - y;
	x = x + y;
	y = x - y;
	x = x - y;

	y2 = p725a_max_y -y2;
	x2 = x2 + y2;
	y2 = x2 - y2;
	x2 = x2 - y2;
#endif


#ifdef TOUCHSCREEN_DUPLICATED_FILTER
	ret = duplicated_filter(ts, x,y,x2,y2, finger2, pressure);
	if (ret == 0) 
	{
					/* printk("%s: duplicated_filter\n", __func__); */
#endif
			
 			 
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, pressure);
			input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, 10);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
			input_mt_sync(ts->input_dev);
			//printk("huangjinyu x = %d ,y= %d pressure = %d \n",x,y,pressure);
			if(finger2)
			{
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, pressure2);
				input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, 10);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x2);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y2);
				//printk("huangjinyu x2 = %d ,y2= %d \n",x2,y2);
				input_mt_sync(ts->input_dev);
			}
			
			input_sync(ts->input_dev);
			 
			
	}
		
#ifdef TOUCHSCREEN_DUPLICATED_FILTER
	}	  	
#endif	
		
		#ifdef POLL_IN_INT
		if(finger)
		{
			hrtimer_start(&ts->timer, ktime_set(0, polling_time), HRTIMER_MODE_REL);
		}
		else
		{
			hrtimer_cancel(&ts->timer);
			enable_irq(ts->client->irq);
		}
		#else
		if (ts->use_irq)
		enable_irq(ts->client->irq);
		#endif
		
}

static enum hrtimer_restart synaptics_ts_timer_func(struct hrtimer *timer)
{
	struct synaptics_ts_data *ts = container_of(timer, struct synaptics_ts_data, timer);

	/* printk("synaptics_ts_timer_func\n"); */

	queue_work(synaptics_wq, &ts->work);
	
	#ifndef POLL_IN_INT
	
	hrtimer_start(&ts->timer, ktime_set(0, polling_time), HRTIMER_MODE_REL);
	
	#endif
	
	return HRTIMER_NORESTART;
}


/*
static enum hrtimer_restart synaptics_ts_resume_func(struct hrtimer *timer)
{
	
	#if 0
	struct synaptics_ts_data *ts = container_of(timer, struct synaptics_ts_data, resume_timer);
	//printk("wly: ts->client->irq=%d\n", ts->client->irq);
    if (ts->use_irq)
		enable_irq(ts->client->irq);
     synaptics_i2c_write(ts->client, 0x26, 0x07);    
	 synaptics_i2c_write(ts->client, 0x31, 0x7F); 
	 #else
	 printk("synaptics_ts_resume_func\n");
	 #endif

	return HRTIMER_NORESTART;
}
*/



static irqreturn_t synaptics_ts_irq_handler(int irq, void *dev_id)
{
	struct synaptics_ts_data *ts = dev_id;

	//pr_info("synaptics_ts_irq_handler\n");
	disable_irq_nosync(ts->client->irq);
	
	#ifdef POLL_IN_INT
	hrtimer_start(&ts->timer, ktime_set(0, 0), HRTIMER_MODE_REL);
	#else
	queue_work(synaptics_wq, &ts->work);
	#endif
	
	return IRQ_HANDLED;
}

static int synaptics_ts_probe(
	struct i2c_client *client, const struct i2c_device_id *id)
{
	struct synaptics_ts_data *ts;
	uint8_t buf1[9];
	//struct i2c_msg msg[2];
	int ret = 0;
	uint16_t max_x, max_y;
	
	struct proc_dir_entry *dir, *refresh;
	
	//printk("sysnaptics probe\n");
	ret = gpio_request(GPIO_TOUCH_EN_OUT, "touch voltage");
	if (ret)
	{	
		printk("gpio 31 request is error!\n");
		goto err_check_functionality_failed;
	}   
	gpio_direction_output(GPIO_TOUCH_EN_OUT, 1);
	msleep(250);
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
	{
		printk(KERN_ERR "synaptics_ts_probe: need I2C_FUNC_I2C\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}
	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (ts == NULL)
	{
		ret = -ENOMEM;
		goto err_alloc_data_failed;
	}

	INIT_WORK(&ts->work, synaptics_ts_work_func);
	
	synaptics_wq = create_singlethread_workqueue("synaptics_swq");
	if(!synaptics_wq)
	{
		ret = -ESRCH;
		pr_err("%s creare single thread workqueue failed!\n", __func__);
		goto err_create_singlethread;
	}
	
	ts->client = client;
	i2c_set_clientdata(client, ts);
	client->driver = &synaptics_ts_driver;
	
	//pdata = client->dev.platform_data;
	//printk("wly:%s, ts->client->addr=%x\n", __FUNCTION__, ts->client->addr);
	{
		int retry = 3;
		while (retry-- > 0)
		{
			#if defined(CONFIG_MACH_SKATE)
			ret = synaptics_i2c_read(ts->client, 0x9E, buf1, 9);
			#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
			ret = synaptics_i2c_read(ts->client, 0x91, buf1, 9);
			#else
			ret = synaptics_i2c_read(ts->client, 0x78, buf1, 9);
			#endif
			printk("wly: synaptics_i2c_read, %c, %c,%c,%c,%c,%c,%c,%c,%c\n",\
				buf1[0],buf1[1],buf1[2],buf1[3],buf1[4],buf1[5],buf1[6],buf1[7],buf1[8]);
			
			if (ret >= 0)
				break;
			msleep(10);
			
		}
		
		
		if (retry < 0)
		{
			ret = -1;
			goto err_detect_failed;
		}
		
	}

	
	#if defined(CONFIG_MACH_SKATE)
	ret = synaptics_i2c_write(ts->client, 0x35, 0x00);
	#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
	ret = synaptics_i2c_write(ts->client, 0x29, 0x00);	
	#elif defined(CONFIG_MACH_MOONCAKE)
	ret = synaptics_i2c_write(client, 0x21, 0x00);			
	#else
	ret = synaptics_i2c_write(ts->client, 0x25, 0x00); /*wly set nomal operation*/
	#endif
	#if defined(CONFIG_MACH_SKATE)
	ret = synaptics_i2c_read(ts->client, 0x3D, buf1, 2);
	#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)||defined(CONFIG_MACH_MOONCAKE)
	ret = synaptics_i2c_read(ts->client, 0x31, buf1, 2);	
	#else
	ret = synaptics_i2c_read(ts->client, 0x2D, buf1, 2);
	#endif
	
	if (ret < 0)
	{
		printk(KERN_ERR "synaptics_i2c_read failed\n");
		goto err_detect_failed;
	}
	ts->max[0] = max_x = buf1[0] | ((buf1[1] & 0x0f) << 8);
	
	#if defined(CONFIG_MACH_SKATE)
	ret = synaptics_i2c_read(ts->client, 0x3F, buf1, 2);
	#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)||defined(CONFIG_MACH_MOONCAKE)
	ret = synaptics_i2c_read(ts->client, 0x33, buf1, 2);	
	#else
	ret = synaptics_i2c_read(ts->client, 0x2F, buf1, 2); 
	#endif
	if (ret < 0)
	{
		printk(KERN_ERR "synaptics_i2c_read failed\n");
		goto err_detect_failed;
	}
	ts->max[1] = max_y = buf1[0] | ((buf1[1] & 0x0f) << 8);
	printk("wly: synaptics_ts_probe,max_x=%d, max_y=%d\n", max_x, max_y);
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		ret = -ENOMEM;
		printk(KERN_ERR "synaptics_ts_probe: Failed to allocate input device\n");
		goto err_input_dev_alloc_failed;
	}
	
	ts->input_dev->name = "synaptics-rmi-touchscreen";
	ts->input_dev->phys = "synaptics-rmi-touchscreen/input0";
	
	
	#ifdef TOUCHSCREEN_DUPLICATED_FILTER
	ts->dup_threshold=(max_y*10/LCD_MAX_Y+5)/10;	
	printk("xuke:dup_threshold %d\n", ts->dup_threshold);
	#endif
	
	
	/*
	ts->input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);
	ts->input_dev->absbit[0] = BIT(ABS_X) | BIT(ABS_Y) | BIT(ABS_PRESSURE);
	ts->input_dev->absbit[BIT_WORD(ABS_MISC)] = BIT_MASK(ABS_MISC);
	ts->input_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);	
	*/
	
	
	set_bit(EV_SYN, ts->input_dev->evbit);
	set_bit(EV_KEY, ts->input_dev->evbit);
	#if defined(CONFIG_TOUCHSCREEN_VIRTUAL_KEYS)
	#if defined(CONFIG_MACH_MOONCAKE)
	set_bit(KEY_HOME,ts->input_dev->keybit);
	set_bit(KEY_BACK,ts->input_dev->keybit);
	#endif
	#endif
	set_bit(BTN_TOUCH, ts->input_dev->keybit);
	set_bit(EV_ABS, ts->input_dev->evbit);
	
	set_bit(ABS_SINGLE_TAP, ts->input_dev->absbit);
	set_bit(ABS_TAP_HOLD, ts->input_dev->absbit);
	set_bit(ABS_EARLY_TAP, ts->input_dev->absbit);
	set_bit(ABS_FLICK, ts->input_dev->absbit);
	set_bit(ABS_PRESS, ts->input_dev->absbit);
	set_bit(ABS_DOUBLE_TAP, ts->input_dev->absbit);
	set_bit(ABS_PINCH, ts->input_dev->absbit);
	//set_bit(ABS_X, ts->input_dev->absbit);
	//set_bit(ABS_Y, ts->input_dev->absbit);
	set_bit(ABS_MT_TOUCH_MAJOR, ts->input_dev->absbit);
	set_bit(ABS_MT_POSITION_X, ts->input_dev->absbit);
	set_bit(ABS_MT_POSITION_Y, ts->input_dev->absbit);
	set_bit(ABS_MT_WIDTH_MAJOR, ts->input_dev->absbit);

#if defined(CONFIG_MACH_R750)
	max_y = 2739;
#endif
	
	//input_set_abs_params(ts->input_dev, ABS_X, 0, max_x, 0, 0);
	//input_set_abs_params(ts->input_dev, ABS_Y, 0, max_y, 0, 0);
	
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

#if defined(CONFIG_MACH_TURIES)
	p725a_max_y = max_y;
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, max_y, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, max_x, 0, 0);
#else
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, max_x, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, max_y, 0, 0);
#endif

	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_SINGLE_TAP, 0, 5, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_TAP_HOLD, 0, 5, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_EARLY_TAP, 0, 5, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_FLICK, 0, 5, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_PRESS, 0, 5, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_DOUBLE_TAP, 0, 5, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_PINCH, -255, 255, 0, 0);
	//input_set_abs_params(ts->input_dev, ABS_PRESSURE, 0, 255, 0, 0);
	
	ret = input_register_device(ts->input_dev);
	if (ret)
	{
		printk(KERN_ERR "synaptics_ts_probe: Unable to register %s input device\n", ts->input_dev->name);
		goto err_input_register_device_failed;
	}

	
	// printk("wly:%s, client->irq=%d\n", __FUNCTION__, client->irq);
	
	
   	//hrtimer_init(&ts->resume_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	//ts->resume_timer.function = synaptics_ts_resume_func;
	
	

	
	
	#ifdef POLL_IN_INT
	hrtimer_init(&ts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	ts->timer.function = synaptics_ts_timer_func;
	ret = request_irq(client->irq, synaptics_ts_irq_handler, IRQF_TRIGGER_FALLING, "synaptics_touch", ts);
	if(ret == 0)
	{
		#if defined(CONFIG_MACH_SKATE)
		ret = synaptics_i2c_write(ts->client, 0x36, 0x07);
		#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
		ret = synaptics_i2c_write(ts->client, 0x2A, 0x07);	
		#elif defined(CONFIG_MACH_MOONCAKE)
		ret = synaptics_i2c_write(client, 0x22, 0x0f);			
		#else
		ret = synaptics_i2c_write(ts->client, 0x26, 0x07);  /* enable abs int ZTE_WLY_CRDB00512790*/
		#endif
		if (ret)
		free_irq(client->irq, ts);
	}
	if(ret == 0)
		ts->use_irq = 1;
	else
		dev_err(&client->dev, "request_irq failed\n");
	#else
	
    
   if (client->irq) 
    {
        ret = request_irq(client->irq, synaptics_ts_irq_handler, IRQF_TRIGGER_FALLING, "synaptics_touch", ts);
		if (ret == 0) {
			#if defined(CONFIG_MACH_SKATE)
			ret = synaptics_i2c_write(ts->client, 0x36, 0x07);
			#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
			ret = synaptics_i2c_write(ts->client, 0x2A, 0x07);	
			#elif defined(CONFIG_MACH_MOONCAKE)
			ret = synaptics_i2c_write(client, 0x22, 0x0f);			
			#else
			ret = synaptics_i2c_write(ts->client, 0x26, 0x07);  /* enable abs int,ZTE_WLY_CRDB00512790 */
			#endif
			if (ret)
				free_irq(client->irq, ts);
		}
		if (ret == 0)
			ts->use_irq = 1;
		else
			dev_err(&client->dev, "request_irq failed\n");
	}
	
    if (!ts->use_irq)
    {
		hrtimer_init(&ts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		ts->timer.function = synaptics_ts_timer_func;
		hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	}
	
	#endif
	
#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	ts->early_suspend.suspend = synaptics_ts_early_suspend;
	ts->early_suspend.resume = synaptics_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif


  dir = proc_mkdir("touchscreen", NULL);
	refresh = create_proc_entry("ts_information", 0644, dir);

	if (refresh) {
		refresh->data		= NULL;
		refresh->read_proc  = proc_read_val;
		refresh->write_proc = proc_write_val;
	}

	printk(KERN_INFO "synaptics_ts_probe: Start touchscreen %s in %s mode\n", ts->input_dev->name, ts->use_irq ? "interrupt" : "polling");

//#ifdef TS_KEY_REPORT
#if defined(CONFIG_TOUCHSCREEN_VIRTUAL_KEYS)
	ts_key_report_init();
#endif

	return 0;

err_input_register_device_failed:
	input_free_device(ts->input_dev);

err_input_dev_alloc_failed:
err_detect_failed:
//err_power_failed:
	kfree(ts);
	destroy_workqueue(synaptics_wq);
err_create_singlethread: 
err_alloc_data_failed:
err_check_functionality_failed:
	gpio_free(GPIO_TOUCH_EN_OUT);
	return ret;
}

static int synaptics_ts_remove(struct i2c_client *client)
{
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
	#if defined(CONFIG_TOUCHSCREEN_VIRTUAL_KEYS)
	ts_key_report_deinit();
	#endif
	unregister_early_suspend(&ts->early_suspend);
	if (ts->use_irq)
		free_irq(client->irq, ts);
	else
		hrtimer_cancel(&ts->timer);
	input_unregister_device(ts->input_dev);
	kfree(ts);
	gpio_direction_output(GPIO_TOUCH_EN_OUT, 0);
	return 0;
}

static int synaptics_ts_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int ret;
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);

	if (ts->use_irq)
		disable_irq(client->irq);
	else
		hrtimer_cancel(&ts->timer);
	
	//hrtimer_cancel(&ts->timer);  //wly
	//hrtimer_cancel(&ts->resume_timer);
	
	ret = cancel_work_sync(&ts->work);
	if (ret && ts->use_irq) /* if work was pending disable-count is now 2 */
		enable_irq(client->irq);
		#if defined(CONFIG_MACH_SKATE)
		ret = synaptics_i2c_write(ts->client, 0x36, 0);
		#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
		ret = synaptics_i2c_write(ts->client, 0x2A, 0);		
		#elif defined(CONFIG_MACH_MOONCAKE)
		ret = synaptics_i2c_write(client, 0x22, 0x00);			
		#else
		ret = synaptics_i2c_write(ts->client, 0x26, 0);     /* disable interrupt,ZTE_WLY_CRDB00512790 */
		#endif
	if (ret < 0)
        printk(KERN_ERR "synaptics_ts_suspend: synaptics_i2c_write failed\n");
		#if defined(CONFIG_MACH_SKATE)
		ret = synaptics_i2c_write(client, 0x35, 0x01);
		#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
		ret = synaptics_i2c_write(client, 0x29, 0x01);			
		#elif defined(CONFIG_MACH_MOONCAKE)
		ret = synaptics_i2c_write(client, 0x21, 0x01);			
		#else
		ret = synaptics_i2c_write(client, 0x25, 0x01);      /* deep sleep *//*wly value need change, ZTE_WLY_CRDB00512790*/
		#endif
	if (ret < 0)
        printk(KERN_ERR "synaptics_ts_suspend: synaptics_i2c_write failed\n");
	//gpio_direction_output(GPIO_TOUCH_EN_OUT, 0);

	return 0;
}

static int synaptics_ts_resume(struct i2c_client *client)
{
	int ret;
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
	gpio_direction_output(GPIO_TOUCH_EN_OUT, 1);
	
	#if defined(CONFIG_MACH_SKATE)
	ret = synaptics_i2c_write(client, 0x35, 0x00);
	#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
	ret = synaptics_i2c_write(ts->client, 0x29, 0x00);		
	#elif defined(CONFIG_MACH_MOONCAKE)
	ret = synaptics_i2c_write(client, 0x21, 0x00);			
	#else
	ret = synaptics_i2c_write(ts->client, 0x25, 0x00); /*wly set nomal operation,ZTE_WLY_CRDB00512790*/
	#endif
	
	if (ts->use_irq)
		enable_irq(client->irq);

	if (!ts->use_irq)
		hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	else
		{
			#if defined(CONFIG_MACH_SKATE)
			synaptics_i2c_write(ts->client, 0x36, 0x07);
			synaptics_i2c_write(ts->client, 0x41, 0x7F);
			#elif defined(CONFIG_TOUCHSCREEN_SYNAPTICS_3K)
			synaptics_i2c_write(ts->client, 0x2A, 0x07); 	
			synaptics_i2c_write(ts->client, 0x35, 0x7F); 	
			#elif defined(CONFIG_MACH_MOONCAKE)
			synaptics_i2c_write(ts->client, 0x22, 0x0f); 	
			//synaptics_i2c_write(ts->client, 0x35, 0x7F); 	
			#else
			synaptics_i2c_write(ts->client, 0x26, 0x07);    /* enable abs int,ZTE_WLY_CRDB00512790 */
			synaptics_i2c_write(ts->client, 0x31, 0x7F); /*wly set 2D gesture enable,ZTE_WLY_CRDB00512790*/
			#endif
		}
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void synaptics_ts_early_suspend(struct early_suspend *h)
{
	struct synaptics_ts_data *ts;
	ts = container_of(h, struct synaptics_ts_data, early_suspend);
	synaptics_ts_suspend(ts->client, PMSG_SUSPEND);
}

static void synaptics_ts_late_resume(struct early_suspend *h)
{
	struct synaptics_ts_data *ts;
	ts = container_of(h, struct synaptics_ts_data, early_suspend);
	synaptics_ts_resume(ts->client);
}
#endif

static const struct i2c_device_id synaptics_ts_id[] = {
	{ SYNAPTICS_I2C_RMI_NAME, 0 },
	{ }
};

static struct i2c_driver synaptics_ts_driver = {
	.probe		= synaptics_ts_probe,
	.remove		= synaptics_ts_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend	= synaptics_ts_suspend,
	.resume		= synaptics_ts_resume,
#endif
	.id_table	= synaptics_ts_id,
	.driver = {
		.name	= SYNAPTICS_I2C_RMI_NAME,
	},
};

static int __devinit synaptics_ts_init(void)
{

	
	#if 0
	synaptics_wq = create_rt_workqueue("synaptics_wq");
	if (!synaptics_wq)
		return -ENOMEM;
	#endif
	
	return i2c_add_driver(&synaptics_ts_driver);
}

static void __exit synaptics_ts_exit(void)
{
	i2c_del_driver(&synaptics_ts_driver);
	if (synaptics_wq)
		destroy_workqueue(synaptics_wq);
}

module_init(synaptics_ts_init);
module_exit(synaptics_ts_exit);

MODULE_DESCRIPTION("Synaptics Touchscreen Driver");
MODULE_LICENSE("GPL");
