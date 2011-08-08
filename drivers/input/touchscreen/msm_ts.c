/* drivers/input/touchscreen/msm_ts.c
 *
 * Copyright (C) 2008 Google, Inc.
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
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
 * TODO:
 *      - Add a timer to simulate a pen_up in case there's a timeout.
 */
 /* 
====================================================================================
when         who        what, where, why                               comment tag
--------     ----          -------------------------------------    ------------------
2010-12-07   wly         modify v9 driver                               ZTE_TS_WLY_CRDB00567837
2010-11-04   wly         ajust zero pressure                            ZTE_TS_WLY_CRDB00567837
2010-05-21   wly         modify debounce time                           ZTE_TS_WLY_0521
2010-03-05   zt          add mod_timer to avoid no rebounce interrupt 	ZTE_TS_ZT_005
2010-01-28   zt          Modified for new BSP								            ZTE_TS_ZT_004
2010-01-28   zt		       Change debounce time							              ZTE_TS_ZT_003
2010-01-28   zt		       Resolve touch no response when power on		    ZTE_TS_ZT_002
2010-01-28   zt		       Update touchscreen driver.						          ZTE_TS_ZT_001
                         register two interrupt and delet virtual key.		
=====================================================================================
*/

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/marimba-tsadc.h>
#include <linux/pm.h>

#if defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#endif

#include <mach/msm_ts.h>
#include <linux/jiffies.h>  

#define TSSC_CTL			0x100
#define 	TSSC_CTL_PENUP_IRQ	(1 << 12)
#define 	TSSC_CTL_DATA_FLAG	(1 << 11)
#define 	TSSC_CTL_DEBOUNCE_EN	(1 << 6)
#define 	TSSC_CTL_EN_AVERAGE	(1 << 5)
#define 	TSSC_CTL_MODE_MASTER	(3 << 3)
#define 	TSSC_CTL_SW_RESET	(1 << 2)
#define 	TSSC_CTL_ENABLE		(1 << 0)
#define TSSC_OPN			0x104
#define 	TSSC_OPN_NOOP		0x00
#define 	TSSC_OPN_4WIRE_X	0x01
#define 	TSSC_OPN_4WIRE_Y	0x02
#define 	TSSC_OPN_4WIRE_Z1	0x03
#define 	TSSC_OPN_4WIRE_Z2	0x04
#define TSSC_SAMPLING_INT		0x108
#define TSSC_STATUS			0x10c
#define TSSC_AVG_12			0x110
#define TSSC_AVG_34			0x114
#define TSSC_SAMPLE(op,samp)		((0x118 + ((op & 0x3) * 0x20)) + \
					 ((samp & 0x7) * 0x4))
#define TSSC_TEST_1			0x198
	#define TSSC_TEST_1_EN_GATE_DEBOUNCE (1 << 2)
#define TSSC_TEST_2			0x19c
#define TS_PENUP_TIMEOUT_MS 70 // 20  -->  70  ZTE_TS_ZT_005 @2010-03-05

struct msm_ts {
	struct msm_ts_platform_data	*pdata;
	struct input_dev		*input_dev;
	void __iomem			*tssc_base;
	uint32_t			ts_down:1;
	uint32_t			zoomhack;
	struct ts_virt_key		*vkey_down;

	unsigned int			sample_irq;
	unsigned int			pen_up_irq;

#if defined(CONFIG_HAS_EARLYSUSPEND)
	struct early_suspend		early_suspend;
#endif
	struct device			*dev;
struct timer_list timer;  //wlyZTE_TS_ZT_005 @2010-03-05
};

static uint32_t msm_tsdebug;
module_param_named(tsdebug, msm_tsdebug, uint, 0664);

//static int32_t msm_tscal_scaler = 65536;
static int32_t msm_tscal_xscale = 34875;
static int32_t msm_tscal_xoffset = -26*65536;
static int32_t msm_tscal_yscale = 58125;
static int32_t msm_tscal_yoffset = 0;
static int32_t msm_tscal_gesture_pressure = 1200;
static int32_t msm_tscal_gesture_blindspot = 100;
//module_param_named(tscal_scaler, msm_tscal_scaler, int, 0664);
module_param_named(tscal_xscale, msm_tscal_xscale, int, 0664);
module_param_named(tscal_xoffset, msm_tscal_xoffset, int, 0664);
module_param_named(tscal_yscale, msm_tscal_yscale, int, 0664);
module_param_named(tscal_yoffset, msm_tscal_yoffset, int, 0664);
module_param_named(tscal_gesture_pressure, msm_tscal_gesture_pressure, int, 0664);
module_param_named(tscal_gesture_blindspot, msm_tscal_gesture_blindspot, int, 0664);

#define tssc_readl(t, a)	(readl(((t)->tssc_base) + (a)))
#define tssc_writel(t, v, a)	do {writel(v, ((t)->tssc_base) + (a));} while(0)

static void setup_next_sample(struct msm_ts *ts)
{
	uint32_t tmp;

	/* 6ms debounce time ,ZTE_TS_WLY_0521*/
	tmp = ((7 << 7) | TSSC_CTL_DEBOUNCE_EN | TSSC_CTL_EN_AVERAGE |
	       TSSC_CTL_MODE_MASTER | TSSC_CTL_ENABLE);

	tssc_writel(ts, tmp, TSSC_CTL);
}

#ifndef CONFIG_TOUCHSCREEN_VIRTUAL_KEYS
static struct ts_virt_key *find_virt_key(struct msm_ts *ts,
					 struct msm_ts_virtual_keys *vkeys,
					 uint32_t val)
{
	int i;

	if (!vkeys)
		return NULL;

	for (i = 0; i < vkeys->num_keys; ++i)
		if ((val >= vkeys->keys[i].min) && (val <= vkeys->keys[i].max))
			return &vkeys->keys[i];
	return NULL;
}
#endif


static void ts_timer(unsigned long arg)
{
	struct msm_ts *ts = (struct msm_ts *)arg;
 	input_report_key(ts->input_dev, BTN_TOUCH, 0);
 	input_sync(ts->input_dev);
}

static irqreturn_t msm_ts_irq(int irq, void *dev_id)
{
	struct msm_ts *ts = dev_id;
	struct msm_ts_platform_data *pdata = ts->pdata;

	uint32_t tssc_avg12, tssc_avg34, tssc_status, tssc_ctl;
	int x, y, z1, z2;
	int was_down;
	int down;
	int z=0;
	del_timer_sync(&ts->timer);
	tssc_ctl = tssc_readl(ts, TSSC_CTL);
	tssc_status = tssc_readl(ts, TSSC_STATUS);
	tssc_avg12 = tssc_readl(ts, TSSC_AVG_12);
	tssc_avg34 = tssc_readl(ts, TSSC_AVG_34);

	setup_next_sample(ts);

	x = tssc_avg12 & 0xffff;
	y = tssc_avg12 >> 16;
	z1 = tssc_avg34 & 0xffff;
	z2 = tssc_avg34 >> 16;

	down = !(tssc_ctl & TSSC_CTL_PENUP_IRQ);
	was_down = ts->ts_down;
	ts->ts_down = down;

	/* no valid data */
	if (down && !(tssc_ctl & TSSC_CTL_DATA_FLAG))
		return IRQ_HANDLED;

	if (msm_tsdebug & 2)
		printk("%s: down=%d, x=%d, y=%d, z1=%d, z2=%d, status %x\n",
		       __func__, down, x, y, z1, z2, tssc_status);
	if (down)
	{
		//if ( 0 == z1 ) return IRQ_HANDLED;
		//z = ( ( 2 * z2 - 2 * z1 - 3) * x) / ( 2 * z1 + 3);
		z = ( ( z2 - z1 - 2)*x) / ( z1 + 2 );
		z = ( 2500 - z ) * 1000 / ( 2500 - 900 );
		//printk("wly: msm_ts_irq,z=%d,z1=%d,z2=%d,x=%d\n",z,z1,z2,x);
		if( z<=0 ) z = 255;
	}

	/* invert the inputs if necessary */
	if (pdata->inv_x) x = pdata->inv_x - x;
	if (pdata->inv_y) y = pdata->inv_y - y;
	if (x < 0) x = 0;
	if (y < 0) y = 0;

	// Calibrate
//        x = (x*msm_tscal_xscale + msm_tscal_xoffset + msm_tscal_scaler/2)/msm_tscal_scaler;
//        y = (y*msm_tscal_yscale + msm_tscal_yoffset + msm_tscal_scaler/2)/msm_tscal_scaler;
        x = (x*msm_tscal_xscale + msm_tscal_xoffset + 32768)/65536;
        y = (y*msm_tscal_yscale + msm_tscal_yoffset + 32768)/65536;


#ifndef CONFIG_TOUCHSCREEN_VIRTUAL_KEYS
	if (!was_down && down) {
		struct ts_virt_key *vkey = NULL;

		if (pdata->vkeys_y && (y > pdata->virt_y_start))
			vkey = find_virt_key(ts, pdata->vkeys_y, x);
		if (!vkey && ts->pdata->vkeys_x && (x > pdata->virt_x_start))
			vkey = find_virt_key(ts, pdata->vkeys_x, y);

		if (vkey) {
			WARN_ON(ts->vkey_down != NULL);
			if(msm_tsdebug)
				printk("%s: virtual key down %d\n", __func__,
				       vkey->key);
			ts->vkey_down = vkey;
			input_report_key(ts->input_dev, vkey->key, 1);
			input_sync(ts->input_dev);
			return IRQ_HANDLED;
		}
	} else if (ts->vkey_down != NULL) {
		if (!down) {
			if(msm_tsdebug)
				printk("%s: virtual key up %d\n", __func__,
				       ts->vkey_down->key);
			input_report_key(ts->input_dev, ts->vkey_down->key, 0);
			input_sync(ts->input_dev);
			ts->vkey_down = NULL;
		}
		return IRQ_HANDLED;
	}
#endif


	if (down) {
		if(z>(msm_tscal_gesture_pressure-msm_tscal_gesture_blindspot) && z<msm_tscal_gesture_pressure) { 	// blind spot (1101-1199)
			down = 0;
			ts->zoomhack = 1;
		} else if(z>=msm_tscal_gesture_pressure) {	// Pinch zoom emulation & pinch rotation emulation
			if(!ts->zoomhack) {			// Flush real position to avoid jumpiness
				down = 0;
				ts->zoomhack = 1;
			}
			else {
				int pinch_radius = (y+1)/2;			// Base pinch radius on y position
				int pinch_x = x - 240;				// Get x offset

				int pinch_y = int_sqrt(pinch_radius*pinch_radius - pinch_x*pinch_x);	// Make sure pinch distance is the same even
													// if we move x around. Pythagoras is our friend.
				if(pinch_y>1000) pinch_y = 0;

				// Finger1
                	        input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 255);
                      		input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, 10);
                        	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, 240 + pinch_x);
                        	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, 400 - pinch_y);
                        	input_mt_sync(ts->input_dev);
				// Finger2
                        	input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 255);
                        	input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, 10);
                        	input_report_abs(ts->input_dev, ABS_MT_POSITION_X, 240 - pinch_x);
                        	input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, 400 + pinch_y);
                        	input_mt_sync(ts->input_dev);
			}
		} else {
			if(ts->zoomhack) {	// Flush faked positions to avoid jumpiness
				down = 0;
				ts->zoomhack = 0;
			} else {
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, (z+1)/2);
                	        input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, 10);
                       		input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
                       		input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
                        	input_mt_sync(ts->input_dev);
			}
		}
	}
	input_report_key(ts->input_dev, BTN_TOUCH, down);
	input_sync(ts->input_dev);

	if (30 == irq)mod_timer(&ts->timer,jiffies + msecs_to_jiffies(TS_PENUP_TIMEOUT_MS));
	return IRQ_HANDLED;
}

static int __devinit msm_ts_hw_init(struct msm_ts *ts)
{
	setup_next_sample(ts);

	return 0;
}

#ifdef CONFIG_PM
static int
msm_ts_suspend(struct device *dev)
{
	struct msm_ts *ts =  dev_get_drvdata(dev);
	uint32_t val;

	disable_irq(ts->sample_irq);
	disable_irq(ts->pen_up_irq);
	val = tssc_readl(ts, TSSC_CTL);
	val &= ~TSSC_CTL_ENABLE;
	tssc_writel(ts, val, TSSC_CTL);

	return 0;
}

static int
msm_ts_resume(struct device *dev)
{
	struct msm_ts *ts =  dev_get_drvdata(dev);

	msm_ts_hw_init(ts);

	enable_irq(ts->sample_irq);
	enable_irq(ts->pen_up_irq);

	return 0;
}

static struct dev_pm_ops msm_touchscreen_pm_ops = {
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend	= msm_ts_suspend,
	.resume		= msm_ts_resume,
#endif
};
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
static void msm_ts_early_suspend(struct early_suspend *h)
{
	struct msm_ts *ts = container_of(h, struct msm_ts, early_suspend);

	msm_ts_suspend(ts->dev);
}

static void msm_ts_late_resume(struct early_suspend *h)
{
	struct msm_ts *ts = container_of(h, struct msm_ts, early_suspend);

	msm_ts_resume(ts->dev);
}
#endif

#if defined(CONFIG_TOUCHSCREEN_VIRTUAL_KEYS)
#define virtualkeys virtualkeys.msm-touchscreen
#if defined(CONFIG_MACH_MOONCAKE)
static const char ts_keys_size[] = "0x01:102:40:340:60:50:0x01:139:120:340:60:50:0x01:158:200:340:60:50";
#elif defined(CONFIG_MACH_V9)
static const char ts_keys_size[] = "0x01:102:70:850:60:50:0x01:139:230:850:60:50:0x01:158:390:850:60:50";
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
#endif

static int __devinit msm_ts_probe(struct platform_device *pdev)
{
	struct msm_ts_platform_data *pdata = pdev->dev.platform_data;
	struct msm_ts *ts;
	struct resource *tssc_res;
	struct resource *irq1_res;
	struct resource *irq2_res;
	int err = 0;
#ifndef CONFIG_TOUCHSCREEN_VIRTUAL_KEYS
	int i;
#endif
	tssc_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tssc");
	irq1_res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "tssc1");
	irq2_res = platform_get_resource_byname(pdev, IORESOURCE_IRQ, "tssc2");

	if (!tssc_res || !irq1_res || !irq2_res) {
		pr_err("%s: required resources not defined\n", __func__);
		return -ENODEV;
	}

	if (pdata == NULL) {
		pr_err("%s: missing platform_data\n", __func__);
		return -ENODEV;
	}

	ts = kzalloc(sizeof(struct msm_ts), GFP_KERNEL);
	if (ts == NULL) {
		pr_err("%s: No memory for struct msm_ts\n", __func__);
		return -ENOMEM;
	}
	ts->pdata = pdata;
	ts->dev	  = &pdev->dev;

	ts->sample_irq = irq1_res->start;
	ts->pen_up_irq = irq2_res->start;

	ts->tssc_base = ioremap(tssc_res->start, resource_size(tssc_res));
	if (ts->tssc_base == NULL) {
		pr_err("%s: Can't ioremap region (0x%08x - 0x%08x)\n", __func__,
		       (uint32_t)tssc_res->start, (uint32_t)tssc_res->end);
		err = -ENOMEM;
		goto err_ioremap_tssc;
	}

	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		pr_err("failed to allocate touchscreen input device\n");
		err = -ENOMEM;
		goto err_alloc_input_dev;
	}
	ts->input_dev->name = "msm-touchscreen";
	ts->input_dev->dev.parent = &pdev->dev;

	input_set_drvdata(ts->input_dev, ts);

	input_set_capability(ts->input_dev, EV_KEY, BTN_TOUCH);
	set_bit(EV_ABS, ts->input_dev->evbit);

	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, pdata->min_x, pdata->max_x, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, pdata->min_y, pdata->max_y, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, pdata->min_press, pdata->max_press, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);

#ifndef CONFIG_TOUCHSCREEN_VIRTUAL_KEYS
	for (i = 0; pdata->vkeys_x && (i < pdata->vkeys_x->num_keys); ++i)
		input_set_capability(ts->input_dev, EV_KEY,
				     pdata->vkeys_x->keys[i].key);
	for (i = 0; pdata->vkeys_y && (i < pdata->vkeys_y->num_keys); ++i)
		input_set_capability(ts->input_dev, EV_KEY,
				     pdata->vkeys_y->keys[i].key);
#endif

	err = input_register_device(ts->input_dev);
	if (err != 0) {
		pr_err("%s: failed to register input device\n", __func__);
		goto err_input_dev_reg;
	}

	setup_timer(&ts->timer, ts_timer, (unsigned long)ts);
	msm_ts_hw_init(ts);

	err = request_irq(ts->sample_irq, msm_ts_irq,
			  (irq1_res->flags & ~IORESOURCE_IRQ) | IRQF_DISABLED,
			  "msm_touchscreen", ts);
	if (err != 0) {
		pr_err("%s: Cannot register irq1 (%d)\n", __func__, err);
		goto err_request_irq1;
	}

	err = request_irq(ts->pen_up_irq, msm_ts_irq,
			  (irq2_res->flags & ~IORESOURCE_IRQ) | IRQF_DISABLED,
			  "msm_touchscreen", ts);
	if (err != 0) {
		pr_err("%s: Cannot register irq2 (%d)\n", __func__, err);
		goto err_request_irq2;
	}

	platform_set_drvdata(pdev, ts);

#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN +
						TSSC_SUSPEND_LEVEL;
	ts->early_suspend.suspend = msm_ts_early_suspend;
	ts->early_suspend.resume = msm_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif

	pr_info("%s: tssc_base=%p irq1=%d irq2=%d\n", __func__,
		ts->tssc_base, (int)ts->sample_irq, (int)ts->pen_up_irq);

#if defined(CONFIG_TOUCHSCREEN_VIRTUAL_KEYS)
	ts_key_report_init();
#endif
	return 0;

err_request_irq2:
	free_irq(ts->sample_irq, ts);

err_request_irq1:
	/* disable the tssc */
	tssc_writel(ts, TSSC_CTL_ENABLE, TSSC_CTL);

err_input_dev_reg:
	input_set_drvdata(ts->input_dev, NULL);
	input_free_device(ts->input_dev);

err_alloc_input_dev:
	iounmap(ts->tssc_base);

err_ioremap_tssc:
	kfree(ts);
	return err;
}

static struct platform_driver msm_touchscreen_driver = {
	.driver = {
		.name = "msm_touchscreen",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &msm_touchscreen_pm_ops,
#endif
	},
	.probe		= msm_ts_probe,
};

static int __init msm_ts_init(void)
{
	return platform_driver_register(&msm_touchscreen_driver);
}

static void __exit msm_ts_exit(void)
{
	platform_driver_unregister(&msm_touchscreen_driver);
}

module_init(msm_ts_init);
module_exit(msm_ts_exit);
MODULE_DESCRIPTION("Qualcomm MSM/QSD Touchscreen controller driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:msm_touchscreen");
