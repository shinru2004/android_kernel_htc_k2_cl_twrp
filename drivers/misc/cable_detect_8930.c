/* drivers/misc/cable_detect.c - cable detect driver
 *
 * Copyright (C) 2009 HTC Corporation.
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/mfd/pmic8058.h>
#include <linux/pmic8058-xoadc.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <mach/board.h>

#ifdef CONFIG_RESET_BY_CABLE_IN
#include <mach/board_htc.h>
#endif

#include <mach/cable_detect.h>
#include <mach/mpp.h>
#include <linux/switch.h>

#ifdef CONFIG_HTC_HEADSET_MGR
#include <mach/htc_headset_mgr.h>
#ifdef CONFIG_HTC_HEADSET_MISC
#include <mach/htc_headset_misc.h>
#endif
#endif

#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
#include "../video/msm_8x60/sii9234/TPI.h"
#endif

#include "linux/mfd/pm8xxx/pm8921-charger.h"

static int vbus;

static struct switch_dev dock_switch = {
	.name = "dock",
};

struct cable_detect_info {
	spinlock_t lock;

	int vbus_mpp_gpio;
	int vbus_mpp_irq;

	
	int ad_en_active_state;
	int ad_en_gpio;
	int ad_en_irq;

	enum usb_connect_type connect_type;
	
	int usb_id_pin_gpio;
	__u8 detect_type;
	__u8 accessory_type;
	int idpin_irq;
	u8 mfg_usb_carkit_enable;
	u8 mhl_reset_gpio;
	bool mhl_version_ctrl_flag;
	struct workqueue_struct *cable_detect_wq;
	struct delayed_work cable_detect_work;
	struct delayed_work vbus_detect_work;
	struct wake_lock vbus_wlock;
	struct wake_lock cable_detect_wlock;
	void (*usb_uart_switch)(int);
	void (*usb_dpdn_switch)(int);
	struct usb_id_mpp_config_data *mpp_data;
	void (*config_usb_id_gpios)(bool enable);
	void (*mhl_1v2_power)(bool enable);
	int (*is_wireless_charger)(void);
	u8 cable_redetect;
	int64_t (*get_adc_cb)(void);

	int ac_9v_gpio;
	void (*configure_ac_9v_gpio) (int);
	u8 mhl_internal_3v3;

	int  audio_dock_lock;
	int notify_init;
} the_cable_info;


#ifdef CONFIG_CABLE_DETECT_ACCESSORY
static int cable_detect_get_adc(void);
static int second_detect(struct cable_detect_info *pInfo);
static void usb_id_detect_init(struct cable_detect_info *info);
#endif

static DEFINE_MUTEX(cable_notify_sem);
static void send_cable_connect_notify(int cable_type)
{
	static struct t_cable_status_notifier *notifier;
	struct cable_detect_info *pInfo = &the_cable_info;

	mutex_lock(&cable_notify_sem);
	CABLE_DEBUG("%s: cable_type = %d\n", __func__, cable_type);

	if (cable_type == CONNECT_TYPE_UNKNOWN)
		cable_type = CONNECT_TYPE_USB;

	if (pInfo->ac_9v_gpio && (cable_type == CONNECT_TYPE_USB
				|| cable_type == CONNECT_TYPE_AC)) {
		if (pInfo->configure_ac_9v_gpio)
			pInfo->configure_ac_9v_gpio(1);

		mdelay(5);
		if (gpio_get_value(pInfo->ac_9v_gpio)) {
			CABLE_INFO("%s detect 9v charger\n", __func__);
			cable_type = CONNECT_TYPE_9V_AC;
		}

		if (pInfo->configure_ac_9v_gpio)
			pInfo->configure_ac_9v_gpio(0);
	}

	if (cable_type > 0 && pInfo->accessory_type == DOCK_STATE_DMB) {
		CABLE_INFO("%s: DMB presents. Disabling charge.\n", __func__);
		cable_type = CONNECT_TYPE_CLEAR;
	}

	list_for_each_entry(notifier,
		&g_lh_calbe_detect_notifier_list,
		cable_notifier_link) {
			if (notifier->func != NULL) {
				CABLE_INFO("Send to: %s, type %d\n",
						notifier->name, cable_type);
				
				
				notifier->func(cable_type);
			}
		}
	mutex_unlock(&cable_notify_sem);
}

int cable_detect_register_notifier(struct t_cable_status_notifier *notifier)
{
	if (!notifier || !notifier->name || !notifier->func)
		return -EINVAL;

	mutex_lock(&cable_notify_sem);
	list_add(&notifier->cable_notifier_link,
		&g_lh_calbe_detect_notifier_list);
	if(the_cable_info.notify_init == 1)
		notifier->func(cable_get_connect_type());
	mutex_unlock(&cable_notify_sem);
	return 0;
}

#if (defined(CONFIG_USB_OTG) && defined(CONFIG_USB_OTG_HOST))
static DEFINE_MUTEX(usb_host_notify_sem);
static void send_usb_host_connect_notify(int cable_in)
{
	struct t_usb_host_status_notifier *notifier;

	mutex_lock(&usb_host_notify_sem);
	list_for_each_entry(notifier,
		&g_lh_usb_host_detect_notifier_list,
		usb_host_notifier_link) {
		if (notifier->func != NULL) {
			CABLE_INFO("[HostNotify] Send to: %s: %d\n",
					notifier->name, cable_in);
			
			
			notifier->func(cable_in);
		}
	}
	mutex_unlock(&usb_host_notify_sem);
}

int usb_host_detect_register_notifier(struct t_usb_host_status_notifier *notifier)
{
	if (!notifier || !notifier->name || !notifier->func)
		return -EINVAL;

	mutex_lock(&usb_host_notify_sem);
	list_add(&notifier->usb_host_notifier_link,
			&g_lh_usb_host_detect_notifier_list);
	mutex_unlock(&usb_host_notify_sem);
	return 0;
}
#endif

static void check_vbus_in(struct work_struct *w)
{
	int vbus_in;
	int level;
	static int isInitial = 1; 
	struct cable_detect_info *pInfo = container_of(
			w, struct cable_detect_info, vbus_detect_work.work);

	level = pm8921_is_usb_chg_plugged_in();
	vbus_in = level;
	CABLE_INFO("%s: vbus = %d, vbus_in = %d\n", __func__, vbus, vbus_in);

#ifdef CONFIG_RESET_BY_CABLE_IN
	reset_dflipflop();
#endif

#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
	if (pInfo->cable_redetect) {
		CABLE_INFO("mhl re-detect\n");
		disable_irq_nosync(pInfo->idpin_irq);
		queue_delayed_work(pInfo->cable_detect_wq,
			&pInfo->cable_detect_work, ADC_DELAY);
	}
#endif

	if (pInfo->notify_init == 0 && vbus_in == 0 && vbus == 0)
		send_cable_connect_notify(CONNECT_TYPE_NONE);
	pInfo->notify_init = 1;

	if (vbus != vbus_in || isInitial) {
		vbus = vbus_in;
		isInitial = 0;
		if (pInfo->usb_uart_switch)
			pInfo->usb_uart_switch(!vbus);
		msm_otg_set_vbus_state(vbus_in);

		if (pInfo->ad_en_gpio) {
			if (vbus) {
				if (pInfo->ad_en_irq)
					CABLE_INFO("%s: Enable ad_en_irq ++\n", __func__);
					enable_irq(pInfo->ad_en_irq);
			} else {
					CABLE_INFO("%s: Disable ad_en_irq --\n", __func__);
					disable_irq_nosync(pInfo->ad_en_irq);
			}
		}
	}
	wake_unlock(&pInfo->vbus_wlock);
}

#ifdef CONFIG_CABLE_DETECT_ACCESSORY
void release_audio_dock_lock(void)
{
	int value;
	struct cable_detect_info *pInfo = &the_cable_info;
	if(pInfo->audio_dock_lock != 1) {
		CABLE_INFO("audio_dock_removal fucntion should not be called when audio_dock_lock != 1\n");
		return;
	}
	CABLE_INFO("unlock audio dock lock\n");

	pInfo->audio_dock_lock = 0;
	
	value = gpio_get_value(pInfo->usb_id_pin_gpio);
	irq_set_irq_type(pInfo->idpin_irq, value ? IRQF_TRIGGER_HIGH: IRQF_TRIGGER_LOW);	
	enable_irq(pInfo->idpin_irq);
}
EXPORT_SYMBOL(release_audio_dock_lock);

static int cable_detect_get_type(struct cable_detect_info *pInfo)
{
	int id_pin, adc, type;
	static int prev_type, stable_count;

	if (stable_count >= ADC_RETRY)
		stable_count = 0;

	id_pin = gpio_get_value(pInfo->usb_id_pin_gpio);
	if (id_pin == 0 || pInfo->cable_redetect) {
		CABLE_INFO("%s: id pin low\n", __func__);


		adc = cable_detect_get_adc();

		if (adc > -100 && adc < 100)
			type = second_detect(pInfo);
		else {
			if (adc > 150 && adc < 220)
				type = DOCK_STATE_CAR;
#ifndef CONFIG_MACH_TC2
			else if (adc > 370 && adc < 440)
				type = DOCK_STATE_USB_HEADSET;
			else if (adc > 440 && adc < 550)
				type = DOCK_STATE_DMB;
#endif
			else if (adc > 550 && adc < 900)
				type = DOCK_STATE_DESK;
			else
				type = DOCK_STATE_UNDEFINED;
		}
	} else {
		CABLE_INFO("%s: id pin high\n", __func__);
		type = DOCK_STATE_UNDOCKED;
	}

	if (prev_type == type)
		stable_count++;
	else
		stable_count = 0;

	CABLE_INFO("%s prev_type %d, type %d, stable_count %d\n",
				__func__, prev_type, type, stable_count);

	prev_type = type;
	return (stable_count >= ADC_RETRY) ? type : -2;
}

static void cable_detect_handler(struct work_struct *w)
{
	struct cable_detect_info *pInfo = container_of(
			w, struct cable_detect_info, cable_detect_work.work);
	int value;
	int accessory_type;

	if (pInfo == NULL)
		return;
	if(pInfo->audio_dock_lock == 1) {
		CABLE_INFO("audio dock lock! skip cable_detect_handler\n");
		return;
	}
#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
	if (pInfo->mhl_reset_gpio != 0)
		gpio_set_value(pInfo->mhl_reset_gpio, 0); 
#endif
	if (pInfo->detect_type == CABLE_TYPE_PMIC_ADC) {
		accessory_type = cable_detect_get_type(pInfo);
		if (accessory_type == -2) {
			queue_delayed_work(pInfo->cable_detect_wq,
				&pInfo->cable_detect_work, ADC_DELAY);
			return;
		}
	} else
		accessory_type = DOCK_STATE_UNDOCKED;

#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
	if (pInfo->mhl_reset_gpio != 0)
		gpio_set_value(pInfo->mhl_reset_gpio, 1); 
	CABLE_INFO("[MHL] Enter D3 mode\n");
	
	if (accessory_type != DOCK_STATE_MHL)
		D2ToD3();
#endif

	if (pInfo->accessory_type == DOCK_STATE_AUDIO_DOCK &&
		accessory_type != DOCK_STATE_UNDEFINED &&
		accessory_type != DOCK_STATE_UNDOCKED) {
		CABLE_INFO("bad accessory state. from audio dock to state %d\n",accessory_type);
		switch_set_state(&dock_switch, DOCK_STATE_UNDOCKED);
#ifdef CONFIG_HTC_HEADSET_MGR
		headset_ext_detect(USB_NO_HEADSET);
#endif
		pInfo->accessory_type = DOCK_STATE_UNDOCKED;
	}

	switch (accessory_type) {
	case DOCK_STATE_DESK:
		CABLE_INFO("cradle inserted\n");
		switch_set_state(&dock_switch, DOCK_STATE_DESK);
		pInfo->accessory_type = DOCK_STATE_DESK;
		break;
	case DOCK_STATE_CAR:
		CABLE_INFO("Car kit inserted\n");
		switch_set_state(&dock_switch, DOCK_STATE_CAR);
		pInfo->accessory_type = DOCK_STATE_CAR;
		break;
	case DOCK_STATE_USB_HEADSET:
		CABLE_INFO("USB headset inserted\n");
		pInfo->accessory_type = DOCK_STATE_USB_HEADSET;
		if (pInfo->usb_dpdn_switch)
			pInfo->usb_dpdn_switch(PATH_USB_AUD);
#ifdef CONFIG_HTC_HEADSET_MGR
		headset_ext_detect(USB_AUDIO_OUT);
#endif
		break;
#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
	case DOCK_STATE_MHL:
		CABLE_INFO("MHL inserted\n");
		switch_set_state(&dock_switch, DOCK_STATE_MHL);
		pInfo->accessory_type = DOCK_STATE_MHL;
#ifdef CONFIG_INTERNAL_CHARGING_SUPPORT
		if (!pInfo->mhl_internal_3v3 && !vbus)
			send_cable_connect_notify(CONNECT_TYPE_INTERNAL);

#endif
		sii9234_mhl_device_wakeup();
		break;
#endif
#if (defined(CONFIG_USB_OTG) && defined(CONFIG_USB_OTG_HOST))
	case DOCK_STATE_USB_HOST:
		CABLE_INFO("USB Host inserted\n");
		send_usb_host_connect_notify(1);
		pInfo->accessory_type = DOCK_STATE_USB_HOST;
		switch_set_state(&dock_switch, DOCK_STATE_USB_HOST);
		break;
#endif
	case DOCK_STATE_DMB:
		CABLE_INFO("DMB inserted\n");
		send_cable_connect_notify(CONNECT_TYPE_CLEAR);
		switch_set_state(&dock_switch, DOCK_STATE_DMB);
		pInfo->accessory_type = DOCK_STATE_DMB;
		break;
	case DOCK_STATE_AUDIO_DOCK:
		CABLE_INFO("Audio Dock inserted\n");
		switch_set_state(&dock_switch, DOCK_STATE_DESK);
		pInfo->accessory_type = DOCK_STATE_AUDIO_DOCK;
#if 0
#ifdef CONFIG_HTC_HEADSET_MGR
		cable_type_value = usb_get_connect_type();
		if (cable_type_value == CONNECT_TYPE_UNKNOWN ||
			cable_type_value == CONNECT_TYPE_USB ||
			cable_type_value == CONNECT_TYPE_AC) {
			CABLE_INFO("notify auido driver in cable_detect_handler, cable type %d\n",cable_type_value);
			pInfo->audio_dock_lock = 1;
			headset_ext_detect(USB_AUDIO_OUT);
			return;	
		}
#endif
#endif
		break;
	case DOCK_STATE_UNDEFINED:
	case DOCK_STATE_UNDOCKED:
		switch (pInfo->accessory_type) {
		case DOCK_STATE_DESK:
			CABLE_INFO("cradle removed\n");
			switch_set_state(&dock_switch, DOCK_STATE_UNDOCKED);
			pInfo->accessory_type = DOCK_STATE_UNDOCKED;
			break;
		case DOCK_STATE_CAR:
			CABLE_INFO("Car kit removed\n");
			switch_set_state(&dock_switch, DOCK_STATE_UNDOCKED);
			pInfo->accessory_type = DOCK_STATE_UNDOCKED;
			break;
		case DOCK_STATE_USB_HEADSET:
			CABLE_INFO("USB headset removed\n");
#ifdef CONFIG_HTC_HEADSET_MGR
			headset_ext_detect(USB_NO_HEADSET);
#endif
			if (pInfo->usb_dpdn_switch)
				pInfo->usb_dpdn_switch(PATH_USB);
			pInfo->accessory_type = DOCK_STATE_UNDOCKED;
			break;
#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
		case DOCK_STATE_MHL:
			CABLE_INFO("MHL removed\n");
			switch_set_state(&dock_switch, DOCK_STATE_UNDOCKED);
			sii9234_disableIRQ();
			break;
#endif
#if (defined(CONFIG_USB_OTG) && defined(CONFIG_USB_OTG_HOST))
		case DOCK_STATE_USB_HOST:
			CABLE_INFO("USB host cable removed\n");
			pInfo->accessory_type = DOCK_STATE_UNDOCKED;
			send_usb_host_connect_notify(0);
			switch_set_state(&dock_switch, DOCK_STATE_UNDOCKED);
			break;
#endif
		case DOCK_STATE_DMB:
			CABLE_INFO("DMB removed\n");
			switch_set_state(&dock_switch, DOCK_STATE_UNDOCKED);
			pInfo->accessory_type = DOCK_STATE_UNDOCKED;
			break;
		case DOCK_STATE_AUDIO_DOCK:
			CABLE_INFO("Audio Dock removed\n");
			switch_set_state(&dock_switch, DOCK_STATE_UNDOCKED);
#ifdef CONFIG_HTC_HEADSET_MGR
			headset_ext_detect(USB_NO_HEADSET);
#endif
			pInfo->accessory_type = DOCK_STATE_UNDOCKED;
			break;
		}
	default :
		break;
	}

	value = gpio_get_value(pInfo->usb_id_pin_gpio);
	CABLE_INFO("%s ID pin %d, type %d\n", __func__,
				value, pInfo->accessory_type);
#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
	if (pInfo->accessory_type == DOCK_STATE_MHL)
		return;
#endif
	if (pInfo->accessory_type == DOCK_STATE_UNDOCKED)
		irq_set_irq_type(pInfo->idpin_irq,
			value ? IRQF_TRIGGER_LOW : IRQF_TRIGGER_HIGH);
	else
		irq_set_irq_type(pInfo->idpin_irq, IRQF_TRIGGER_HIGH);

	enable_irq(pInfo->idpin_irq);
#if 0
	wake_unlock(&pInfo->cable_detect_wlock);
#endif
}

void set_mfg_usb_carkit_enable(int enable)
{
	the_cable_info.mfg_usb_carkit_enable = enable;
}

int cable_get_accessory_type(void)
{
	return the_cable_info.accessory_type;
}

int check_three_pogo_dock(void)
{
	return 0;
}

static int cable_detect_get_adc(void)
{
	struct cable_detect_info *pInfo = &the_cable_info;

	return pInfo->get_adc_cb();
}

int cable_get_usb_id_level(void)
{
	struct cable_detect_info *pInfo = &the_cable_info;

	if (pInfo->usb_id_pin_gpio)
		return gpio_get_value(pInfo->usb_id_pin_gpio);
	else {
		printk(KERN_INFO "usb id is not defined\n");
		return 1;
	}
}

static int second_detect(struct cable_detect_info *pInfo)
{
	uint32_t adc_value = 0xffffffff;
	int type;

	if (pInfo->config_usb_id_gpios)
		pInfo->config_usb_id_gpios(1);

	adc_value = cable_detect_get_adc();
	CABLE_INFO("[2nd] accessory adc = %d\n", adc_value);

	if ((pInfo->mhl_version_ctrl_flag) || (adc_value >= 776 && adc_value <= 1020))
#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
		type = DOCK_STATE_MHL;
#else
		type = DOCK_STATE_UNDEFINED;
#endif
	else if(adc_value >= 1021 && adc_value <= 1224)
		type = DOCK_STATE_AUDIO_DOCK;
	else
#if (defined(CONFIG_USB_OTG) && defined(CONFIG_USB_OTG_HOST))
		type = DOCK_STATE_USB_HOST;
#else
		type = DOCK_STATE_UNDEFINED;
#endif

	if (pInfo->config_usb_id_gpios)
		pInfo->config_usb_id_gpios(0);

	return type;
}

static int get_usb_id_adc(char *buffer, struct kernel_param *kp)
{
	unsigned length = 0;
	int adc;

	adc = cable_detect_get_adc();

	length += sprintf(buffer, "%d\n", adc);

	return length;
}
module_param_call(usb_id_adc, NULL, get_usb_id_adc, NULL, 0664);

static ssize_t dock_status_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct cable_detect_info *pInfo = &the_cable_info;

	if (pInfo->accessory_type == DOCK_STATE_DESK || pInfo->accessory_type == DOCK_STATE_AUDIO_DOCK)
		return sprintf(buf, "online\n");
	else if (pInfo->accessory_type == 3) 
		return sprintf(buf, "online\n");
	else
		return sprintf(buf, "offline\n");
}
static DEVICE_ATTR(status, S_IRUGO | S_IWUSR, dock_status_show, NULL);

static irqreturn_t usbid_interrupt(int irq, void *data)
{
	struct cable_detect_info *pInfo = (struct cable_detect_info *)data;

	disable_irq_nosync(pInfo->idpin_irq);

	CABLE_INFO("usb: id interrupt\n");
	pInfo->cable_redetect = 0;
	queue_delayed_work(pInfo->cable_detect_wq,
		&pInfo->cable_detect_work, ADC_DELAY);
	wake_lock_timeout(&pInfo->cable_detect_wlock, HZ*2);
	return IRQ_HANDLED;
}

static void usb_id_detect_init(struct cable_detect_info *pInfo)
{
	int ret;
	CABLE_INFO("%s: id pin %d\n", __func__,
		pInfo->usb_id_pin_gpio);

	if (pInfo->usb_id_pin_gpio == 0)
		return;
	ret = gpio_request(pInfo->usb_id_pin_gpio, "USBID_GPIO");
	if (ret) {
		CABLE_ERR("%s: request id gpio failed\n", __func__);
		return;
	}
	if (pInfo->idpin_irq == 0)
		pInfo->idpin_irq = gpio_to_irq(pInfo->usb_id_pin_gpio);

	set_irq_flags(pInfo->idpin_irq, IRQF_VALID | IRQF_NOAUTOEN);
	ret = request_any_context_irq(pInfo->idpin_irq, usbid_interrupt,
				IRQF_TRIGGER_LOW, "idpin_irq", pInfo);
	if (ret < 0) {
		CABLE_ERR("%s: request_irq failed\n", __func__);
		return;
	}

	ret = enable_irq_wake(pInfo->idpin_irq);
	if (ret < 0) {
		CABLE_ERR("%s: set_irq_wake failed\n", __func__);
		goto err;
	}

	enable_irq(pInfo->idpin_irq);
	return;
err:
	free_irq(pInfo->idpin_irq, 0);
}

#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
static void mhl_status_notifier_func(bool isMHL, int charging_type)
{
	struct cable_detect_info *pInfo = &the_cable_info;
	int id_pin = gpio_get_value(pInfo->usb_id_pin_gpio);
	static uint8_t mhl_connected;

	CABLE_INFO("%s: isMHL %d, charging type %d, id_pin %d\n",
				__func__, isMHL, charging_type, id_pin);
	if (pInfo->accessory_type != DOCK_STATE_MHL) {
		CABLE_INFO("%s: accessory is not MHL, type %d\n",
					__func__, pInfo->accessory_type);
		return;
	}

#ifdef CONFIG_HTC_HEADSET_MISC
	headset_mhl_audio_jack_enable(isMHL);
#endif

	if (!isMHL) {
		CABLE_INFO("MHL removed\n");
		sii9234_disableIRQ();

		if (pInfo->usb_dpdn_switch)
			pInfo->usb_dpdn_switch(PATH_USB);

		if (pInfo->mhl_1v2_power)
			pInfo->mhl_1v2_power(0);
#ifdef CONFIG_INTERNAL_CHARGING_SUPPORT
		send_cable_connect_notify(CONNECT_TYPE_CLEAR);
#endif
#ifdef MHL_REDETECT
		if (mhl_connected == 0) {
			CABLE_INFO("MHL re-detect\n");
			set_irq_type(pInfo->idpin_irq,
				id_pin ? IRQF_TRIGGER_LOW : IRQF_TRIGGER_HIGH);
			pInfo->cable_redetect = 1;
		}
#endif
		mhl_connected = 0;
		switch_set_state(&dock_switch, DOCK_STATE_UNDOCKED);
		pInfo->accessory_type = DOCK_STATE_UNDOCKED;
		sii9234_disableIRQ();
		enable_irq(pInfo->idpin_irq);
		return;
	} else {
		mhl_connected = 1;
		set_irq_type(pInfo->idpin_irq,
			id_pin ? IRQF_TRIGGER_LOW : IRQF_TRIGGER_HIGH);
		if (vbus && (charging_type > CONNECT_TYPE_NONE)) {
#ifdef CONFIG_INTERNAL_CHARGING_SUPPORT
			if (charging_type == CONNECT_TYPE_INTERNAL)
				charging_type = CONNECT_TYPE_NONE;
			send_cable_connect_notify(charging_type);
#else
			send_cable_connect_notify(charging_type);
#endif
		}
#if 0
#ifdef CONFIG_INTERNAL_CHARGING_SUPPORT
		else if (vbus)
			send_cable_connect_notify(CONNECT_TYPE_USB);
#endif
#endif
	}
}

static struct t_mhl_status_notifier mhl_status_notifier = {
	.name = "mhl_detect",
	.func = mhl_status_notifier_func,
};
#endif 
#endif 


static ssize_t vbus_status_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int level, vbus_in;

	level = pm8921_is_usb_chg_plugged_in();
	vbus_in = level;
	CABLE_INFO("%s: vbus state = %d\n", __func__, vbus_in);
	return sprintf(buf, "%d\n", vbus_in);
}
static DEVICE_ATTR(vbus, S_IRUGO | S_IWUSR, vbus_status_show, NULL);

#ifdef CONFIG_CABLE_DETECT_ACCESSORY
static ssize_t adc_status_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int adc;

	adc = cable_detect_get_adc();
	CABLE_INFO("%s: ADC = %d\n", __func__, adc);
	return sprintf(buf, "%d\n", adc);
}
static DEVICE_ATTR(adc, S_IRUGO | S_IWUSR, adc_status_show, NULL);

static ssize_t dmb_wakeup_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct cable_detect_info *pInfo = &the_cable_info;
	uint32_t wakeup;

	if (pInfo->accessory_type != DOCK_STATE_DMB) {
		CABLE_INFO("%s: DMB not exist. Do nothing.\n", __func__);
		return count;
	}

	sscanf(buf, "%d", &wakeup);
	CABLE_DEBUG("%s: wakeup = %d\n", __func__, wakeup);
	if (!!wakeup) {
		disable_irq_nosync(pInfo->idpin_irq);

		gpio_direction_output(pInfo->usb_id_pin_gpio, 0);
		msleep(1);
		gpio_direction_output(pInfo->usb_id_pin_gpio, 1);
		msleep(10);
		gpio_direction_output(pInfo->usb_id_pin_gpio, 0);
		msleep(1);

		gpio_direction_input(pInfo->usb_id_pin_gpio);
		enable_irq(pInfo->idpin_irq);
	}
	CABLE_INFO("%s(parent:%s): request DMB wakeup done.\n",
			current->comm, current->parent->comm);

	return count;
}

static DEVICE_ATTR(dmb_wakeup, S_IRUGO | S_IWUSR, NULL, dmb_wakeup_store);
static ssize_t unlock_audio_dock_lock_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	uint32_t unlock;

	sscanf(buf, "%d", &unlock);
	CABLE_DEBUG("%s: unlock = %d\n", __func__, unlock);
	if (!!unlock)
		release_audio_dock_lock();

	return count;
}

static DEVICE_ATTR(unlock_audio_dock_lock, S_IRUGO | S_IWUSR, NULL, unlock_audio_dock_lock_store);

#endif

int cable_get_connect_type(void)
{
	struct cable_detect_info *pInfo = &the_cable_info;

	return pInfo->connect_type;
}

static irqreturn_t ad_en_irq_handler(int irq, void *data)
{
	unsigned long flags;
	struct cable_detect_info *pInfo = &the_cable_info;

	disable_irq_nosync(pInfo->ad_en_irq);
	CABLE_INFO("%s: Disable ad_en_irq --\n", __func__);
	spin_lock_irqsave(&pInfo->lock, flags);
	queue_delayed_work(pInfo->cable_detect_wq,
			&pInfo->vbus_detect_work, HZ/10);
	spin_unlock_irqrestore(&pInfo->lock, flags);
#if 1
	wake_lock_timeout(&pInfo->vbus_wlock, HZ*2);
#endif

	return IRQ_HANDLED;
}

static int cd_pmic_request_irq(unsigned int gpio, unsigned int *irq,
			       irq_handler_t handler, unsigned long flags,
			       const char *name, unsigned int wake)
{
	int ret = 0;

	ret = gpio_request(gpio, name);
	if (ret < 0)
		return ret;

	ret = gpio_direction_input(gpio);
	if (ret < 0) {
		gpio_free(gpio);
		return ret;
	}

	if (!(*irq)) {
		ret = gpio_to_irq(gpio);
		if (ret < 0) {
			gpio_free(gpio);
			return ret;
		}
		*irq = (unsigned int) ret;
	}

	ret = request_any_context_irq(*irq, handler, flags, name, NULL);
	if (ret < 0) {
		gpio_free(gpio);
		return ret;
	}

	ret = irq_set_irq_wake(*irq, wake);
	if (ret < 0) {
		free_irq(*irq, 0);
		gpio_free(gpio);
		return ret;
	}

	return 1;
}

static int cable_detect_probe(struct platform_device *pdev)
{
	int ret;
	struct cable_detect_platform_data *pdata = pdev->dev.platform_data;
	struct cable_detect_info *pInfo = &the_cable_info;

	spin_lock_init(&the_cable_info.lock);

	if (pdata) {
		pInfo->vbus_mpp_gpio = pdata->vbus_mpp_gpio;
		pInfo->vbus_mpp_irq = pdata->vbus_mpp_irq;
		pInfo->ad_en_active_state = pdata->ad_en_active_state;
		pInfo->ad_en_gpio = pdata->ad_en_gpio;
		pInfo->ad_en_irq = pdata->ad_en_irq;
		pInfo->usb_uart_switch = pdata->usb_uart_switch;
		pInfo->usb_dpdn_switch = pdata->usb_dpdn_switch;
		if (pInfo->usb_dpdn_switch)
			pInfo->usb_dpdn_switch(PATH_USB);
		pInfo->ac_9v_gpio = pdata->ac_9v_gpio;
		pInfo->configure_ac_9v_gpio = pdata->configure_ac_9v_gpio;
		pInfo->mhl_internal_3v3 = pdata->mhl_internal_3v3;

#ifdef CONFIG_CABLE_DETECT_ACCESSORY
		pInfo->detect_type = pdata->detect_type;
		pInfo->usb_id_pin_gpio = pdata->usb_id_pin_gpio;
		pInfo->mhl_reset_gpio = pdata->mhl_reset_gpio;
		pInfo->mpp_data = &pdata->mpp_data;
		pInfo->config_usb_id_gpios = pdata->config_usb_id_gpios;
		pInfo->mhl_version_ctrl_flag = pdata->mhl_version_ctrl_flag;
		pInfo->mhl_1v2_power = pdata->mhl_1v2_power;
		pInfo->get_adc_cb = pdata->get_adc_cb;

		if (pInfo->config_usb_id_gpios)
			pInfo->config_usb_id_gpios(0);
#endif

		if (pdata->is_wireless_charger)
			pInfo->is_wireless_charger = pdata->is_wireless_charger;
#ifdef CONFIG_CABLE_DETECT_ACCESSORY
		INIT_DELAYED_WORK(&pInfo->cable_detect_work, cable_detect_handler);
#endif
		INIT_DELAYED_WORK(&pInfo->vbus_detect_work, check_vbus_in);

		pInfo->cable_detect_wq = create_singlethread_workqueue("cable_detect");
		if (pInfo->cable_detect_wq == 0) {
			CABLE_ERR("usb: fail to create workqueue\n");
			return -ENOMEM;
		}

		if (pdata->vbus_mpp_config)
			pdata->vbus_mpp_config();

		wake_lock_init(&pInfo->vbus_wlock,
			WAKE_LOCK_SUSPEND, "vbus_lock");
		wake_lock_init(&pInfo->cable_detect_wlock,
			WAKE_LOCK_SUSPEND, "cable_detect_lock");

		if (pdata->ad_en_gpio) {
			ret = cd_pmic_request_irq(pdata->ad_en_gpio,
					&pdata->ad_en_irq, ad_en_irq_handler,
					pdata->ad_en_active_state ?
					IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING,
					"ad_en_irq", 1);
			if (ret < 0) {
				printk("Failed to request PMIC AD_EN IRQ (0x%X)", ret);
			} else
				disable_irq(pdata->ad_en_irq);
		}
	}
	if (switch_dev_register(&dock_switch) < 0) {
		CABLE_ERR("fail to register dock switch!\n");
		return 0;
	}

	ret = device_create_file(dock_switch.dev, &dev_attr_vbus);
	if (ret != 0)
		CABLE_ERR("dev_attr_vbus failed\n");

#ifdef CONFIG_CABLE_DETECT_ACCESSORY
	ret = device_create_file(dock_switch.dev, &dev_attr_status);
	if (ret != 0)
		CABLE_ERR("dev_attr_status failed\n");

	ret = device_create_file(dock_switch.dev, &dev_attr_adc);
	if (ret != 0)
		CABLE_ERR("dev_attr_adc failed\n");

	ret = device_create_file(dock_switch.dev, &dev_attr_dmb_wakeup);
	if (ret != 0)
		CABLE_ERR("dev_attr_dmb_wakeup failed\n");

	ret = device_create_file(dock_switch.dev, &dev_attr_unlock_audio_dock_lock);
	if (ret != 0)
		CABLE_ERR("dev_attr_unlock_audio_dock_lock failed\n");

	usb_id_detect_init(pInfo);
#endif

	return 0;
}

irqreturn_t cable_detection_vbus_irq_handler(void)
{
	unsigned long flags;
	struct cable_detect_info *pInfo = &the_cable_info;

	CABLE_INFO("%s\n", __func__);
	spin_lock_irqsave(&pInfo->lock, flags);
	queue_delayed_work(pInfo->cable_detect_wq,
			&pInfo->vbus_detect_work, HZ/10);
	spin_unlock_irqrestore(&pInfo->lock, flags);
#if 1
	wake_lock_timeout(&pInfo->vbus_wlock, HZ*2);
#endif

	return IRQ_HANDLED;
}
EXPORT_SYMBOL(cable_detection_vbus_irq_handler);

struct platform_driver cable_detect_driver = {
	.probe = cable_detect_probe,
	
	.driver = {
		.name	= "cable_detect",
		.owner = THIS_MODULE,
	},
};

static void usb_status_notifier_func(int cable_type)
{
	struct cable_detect_info*pInfo = &the_cable_info;

	CABLE_INFO("%s: cable_type = %d\n", __func__, cable_type);

	
	if(pInfo->audio_dock_lock == 0 && (cable_type == CONNECT_TYPE_USB || cable_type == CONNECT_TYPE_AC))
		if(pInfo->accessory_type == DOCK_STATE_AUDIO_DOCK) {
#ifdef CONFIG_HTC_HEADSET_MGR
			CABLE_INFO("notify auido driver in usb_status_notifier_func\n");
			pInfo->audio_dock_lock = 1;
			
			cancel_delayed_work_sync(&pInfo->cable_detect_work);
			if (pInfo->accessory_type == DOCK_STATE_AUDIO_DOCK)
				headset_ext_detect(USB_AUDIO_OUT);
			else {
				CABLE_INFO("latest accessory type is %d, stop to notify audio dock\n",pInfo->accessory_type);
				pInfo->audio_dock_lock = 0;
			}
#endif
		}

	if (cable_type > CONNECT_TYPE_NONE) {
		if (pInfo->ad_en_gpio) {
			if (gpio_get_value(pInfo->ad_en_gpio) ==
							pInfo->ad_en_active_state)
				cable_type = CONNECT_TYPE_WIRELESS;
		} else if (pInfo->is_wireless_charger) {
			if (pInfo->is_wireless_charger())
				cable_type = CONNECT_TYPE_WIRELESS;
		}
	}

#ifdef CONFIG_FB_MSM_HDMI_MHL_SII9234
#ifdef CONFIG_INTERNAL_CHARGING_SUPPORT
	if (!pInfo->mhl_internal_3v3 &&
			pInfo->accessory_type == DOCK_STATE_MHL) {
		CABLE_INFO("%s: MHL detected. Do nothing\n", __func__);
		return;
	}
#endif
#endif
	pInfo->connect_type = cable_type;
	send_cable_connect_notify(cable_type);

}

static struct t_usb_status_notifier usb_status_notifier = {
	.name = "cable_detect",
	.func = usb_status_notifier_func,
};

static int __init cable_detect_init(void)
{
	vbus = 0;
	the_cable_info.connect_type = CONNECT_TYPE_NONE;
	
	htc_usb_register_notifier(&usb_status_notifier);
#if (defined(CONFIG_CABLE_DETECT_ACCESSORY) && defined(CONFIG_FB_MSM_HDMI_MHL_SII9234))
		mhl_detect_register_notifier(&mhl_status_notifier);
#endif
	return platform_driver_register(&cable_detect_driver);

}

static void __exit cable_detect_exit(void)
{
	platform_driver_unregister(&cable_detect_driver);
}

MODULE_DESCRIPTION("CABLE_DETECT");
MODULE_LICENSE("GPL");

module_init(cable_detect_init);
module_exit(cable_detect_exit);
