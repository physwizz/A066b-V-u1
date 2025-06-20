#define pr_fmt(fmt) "nopmi_chg_jeita %s: " fmt, __func__

#include "nopmi_chg_jeita.h" 
#include "nopmi_chg_iio.h"
#include "nopmi_chg.h"
#include <linux/err.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>
////#include <linux/ipc_logging.h>
#include <linux/printk.h>
#include "../../misc/mediatek/typec/tcpc/inc/tcpm.h"

//#define BATT_VOL_USE_CP

#if 0
#undef pr_debug
#define pr_debug pr_err
#undef pr_info
#define pr_info pr_err
#endif
struct nopmi_chg_jeita_st *g_nopmi_chg_jeita = NULL;

#define JEITA_CHG_VOTER		"JEITA_CHG_VOTER"

extern struct nopmi_chg *g_nopmi_chg;
bool g_ffc_disable = true;
EXPORT_SYMBOL_GPL(g_ffc_disable);

////extern bool first_power_on;

//extern int main_set_charge_enable(bool en);
//extern int adapter_dev_get_pd_verified(void);
//add ipc log start
#if IS_ENABLED(CONFIG_FACTORY_BUILD)
	#if IS_ENABLED(CONFIG_DEBUG_OBJECTS)
		#define IPC_CHARGER_DEBUG_LOG
	#endif
#endif

#ifdef IPC_CHARGER_DEBUG_LOG
extern void *charger_ipc_log_context;
#undef pr_err
#define pr_err(_fmt, ...) \
	{ \
		if(!charger_ipc_log_context){   \
			printk(KERN_ERR pr_fmt(_fmt), ##__VA_ARGS__);    \
		}else{                                             \
			ipc_log_string(charger_ipc_log_context, "nopmi_jeita: %s %d "_fmt, __func__, __LINE__, ##__VA_ARGS__); \
		}\
	}
#undef pr_info
#define pr_info(_fmt, ...) \
	{ \
		if(!charger_ipc_log_context){   \
			printk(KERN_INFO pr_fmt(_fmt), ##__VA_ARGS__);    \
		}else{                                             \
			ipc_log_string(charger_ipc_log_context, "nopmi_jeita: %s %d "_fmt, __func__, __LINE__, ##__VA_ARGS__); \
		}\
	}
#endif
//add ipc log end

#define JEITA_STEP_VOLTAGE_HYSTERISIS_MV  3
#define TAPERED_FCC_REDUCTION_STEP_MA  100

static void continue_nopmi_chg_jeita_workfunc(void);

static int nopmi_chg_jeita_get_bat_temperature(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret = 0;
	int temp = 0;

	/*if(first_power_on == true) {
		temp = 30;
		first_power_on = false;
		pr_info("this is first power on[%d]",first_power_on);
		return temp;
	}*/

    if(!nopmi_chg_jeita->bms_psy)
    {
    	nopmi_chg_jeita->bms_psy = power_supply_get_by_name("battery");
        if (!nopmi_chg_jeita->bms_psy) {
        	pr_err("bms supply not found, defer probe\n");
        	return -EINVAL;
        }
	}

	ret = power_supply_get_property(nopmi_chg_jeita->bms_psy,
				POWER_SUPPLY_PROP_TEMP, &prop);
	if (ret < 0) {
		pr_err("couldn't read temperature property, ret=%d\n", ret);
		return -EINVAL;
	}
	temp = prop.intval/10;

	//pr_info("get_bat_temperature is %d\n", temp);
	return temp;
}

static int nopmi_chg_jeita_get_battery_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int rc = 0;
	union power_supply_propval pval = {0, };

	if(!nopmi_chg_jeita->bms_psy)
		nopmi_chg_jeita->bms_psy = power_supply_get_by_name("battery");
	if (!nopmi_chg_jeita->bms_psy) {
		pr_err("bms supply not found\n");
		return -EINVAL;
	}
	rc = power_supply_get_property(nopmi_chg_jeita->bms_psy, POWER_SUPPLY_PROP_CURRENT_NOW, &pval);
	if (rc < 0) {
		pr_err("Couldn't get battery current\n");
		return -EINVAL;
	}

	return pval.intval/1000;
}

static int nopmi_chg_jeita_get_battery_voltage(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int rc = 0;
	int c_plus_voltage = 0;
	union power_supply_propval pval = {0, };

#ifdef BATT_VOL_USE_CP
	rc = nopmi_chg_get_iio_channel(g_nopmi_chg,
			NOPMI_CP, CHARGE_PUMP_SP_BATTERY_VOLTAGE, &c_plus_voltage);
	if (rc < 0) {
		pr_err("Couldn't get iio c_plus_voltage\n");
		return -ENODATA;
	}
	//pr_err("get charger_voltage is %d\n", c_plus_voltage);
#else
	if(!nopmi_chg_jeita->bms_psy)
		nopmi_chg_jeita->bms_psy = power_supply_get_by_name("battery");
	if (!nopmi_chg_jeita->bms_psy) {
		pr_err("bms supply not found\n");
		return -EINVAL;
	}
	rc = power_supply_get_property(nopmi_chg_jeita->bms_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
	if (rc < 0) {
		pr_err("Couldn't get battery voltage\n");
		return -EINVAL;
	}
	c_plus_voltage = pval.intval/1000;
#endif

	return c_plus_voltage;
}
static int nopmi_chg_jeita_get_charger_voltage(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret = 0;
	int voltage;

    if(!nopmi_chg_jeita->bbc_psy)
    {
     	nopmi_chg_jeita->bbc_psy = power_supply_get_by_name("charger");
        if (!nopmi_chg_jeita->bbc_psy) {
        	pr_err("bbc supply not found, defer probe\n");
        	return -EINVAL;
        }
	}
	
	ret = power_supply_get_property(nopmi_chg_jeita->bbc_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &prop);
	if (ret < 0) {
		pr_err("couldn't read voltage property, ret=%d\n", ret);
		return -EINVAL;
	}
	voltage = prop.intval;
	//pr_info("get charger_voltage is %d\n", voltage);
	return voltage;
}

static int nopmi_chg_jeita_get_batt_id(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
    struct nopmi_chg *chg = container_of(nopmi_chg_jeita, struct nopmi_chg, jeita_ctl);
	int rc = 0;
	int batt_id = 0;

    rc = nopmi_chg_get_iio_channel(chg, NOPMI_BMS, FG_RESISTANCE_ID, &batt_id);
	if (rc != 0) {
		pr_err("couldn't get batt_id property, ret=%d\n", rc);
		return -EINVAL;
	}
	pr_info("get batt_id: %d\n", batt_id);
	return batt_id;
}

static int nopmi_chg_jeita_get_pd_active(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int pd_active = 0;
	int rc = 0;
	if (!g_nopmi_chg->tcpc_dev)
		pr_err("tcpc_dev is null\n");
	else {
		rc = tcpm_get_pd_connect_type(g_nopmi_chg->tcpc_dev);
		pr_err("%s: pd is %d\n", __func__, rc);
		if ((rc == PD_CONNECT_PE_READY_SNK_APDO) || (rc == PD_CONNECT_PE_READY_SNK_PD30)
			|| (rc == PD_CONNECT_PE_READY_SNK)) {
			pd_active = 1;
		} else {
			pd_active = 0;
		}
	}

	return pd_active;
}

static int nopmi_chg_jeita_get_afc_active(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int rc = 0;
	int charge_afc = 0;
	rc = nopmi_chg_get_iio_channel(g_nopmi_chg,
			NOPMI_MAIN, MAIN_CHARGE_AFC, &charge_afc);
	if (rc < 0) {
		pr_err("Couldn't get iio usb_ma\n");
		return -ENODATA;
	}

	return charge_afc;
}

static int nopmi_chg_jeita_get_charger_term_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret = 0;
	int term_curr = 0;

	nopmi_chg_jeita->bbc_psy = power_supply_get_by_name("charger");
	if (!nopmi_chg_jeita->bbc_psy) {
		pr_err("usb supply not found, defer probe\n");
		return -EINVAL;
	}
	ret = power_supply_get_property(nopmi_chg_jeita->bbc_psy,
				POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT, &prop);
	if (ret < 0) {
		pr_err("couldn't get term_current property, ret=%d\n", ret);
		return -EINVAL;
	}
	term_curr = prop.intval;
	//pr_info("get term_current: %d\n", prop.intval);
	return term_curr;
}

static int nopmi_chg_jeita_set_charger_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita, int cc)
{
	union power_supply_propval prop = {0, };
	int ret = 0;
	nopmi_chg_jeita->bbc_psy = power_supply_get_by_name("charger");
	if (!nopmi_chg_jeita->bbc_psy) {
		pr_err("bbc supply not found, defer probe\n");
		return -EINVAL;
	}
	prop.intval = cc;
	ret = power_supply_set_property(nopmi_chg_jeita->bbc_psy,
				POWER_SUPPLY_PROP_CURRENT_NOW, &prop);

	if (ret < 0) {
		pr_err("couldn't set current property, ret=%d\n", ret);
		return -EINVAL;
	}

	pr_info("set current is %d\n", prop.intval);
	return 0;
}

static int nopmi_chg_jeita_set_charger_voltage(struct nopmi_chg_jeita_st *nopmi_chg_jeita, int cv)
{
	union power_supply_propval prop = {0, };
	int ret = 0;

	nopmi_chg_jeita->bbc_psy = power_supply_get_by_name("charger");
	if (!nopmi_chg_jeita->bbc_psy) {
		pr_err("bbc supply not found, defer probe\n");
		return -EINVAL;
	}
	prop.intval = cv;
	ret = power_supply_set_property(nopmi_chg_jeita->bbc_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &prop);
	if (ret < 0) {
		pr_err("couldn't set voltage property, ret=%d\n", ret);
		return -EINVAL;
	}
	pr_info("set voltage is %d\n", prop.intval);
	return 0;
}

static int nopmi_chg_jeita_set_charger_term_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita, int term_curr)
{
	union power_supply_propval prop = {0, };
	int ret = 0;

	nopmi_chg_jeita->bbc_psy = power_supply_get_by_name("charger");
	if (!nopmi_chg_jeita->bbc_psy) {
		pr_err("usb supply not found, defer probe\n");
		return -EINVAL;
	}
	prop.intval = term_curr;
	ret = power_supply_set_property(nopmi_chg_jeita->bbc_psy,
				POWER_SUPPLY_PROP_CHARGE_TERM_CURRENT, &prop);
	if (ret < 0) {
		pr_err("couldn't set term_current property, ret=%d\n", ret);
		return -EINVAL;
	}
	pr_info("set term_current is %d\n", prop.intval);
	return 0;
}

/*static int nopmi_chg_jeita_set_charger_enabled(struct nopmi_chg_jeita_st *nopmi_chg_jeita, bool enabled)
{
	
	union power_supply_propval prop = {0, };
	int ret = 0;
	
	pr_err("2021.09.10 wsy start %s:enabled is %d\n", __func__, enabled);
	nopmi_chg_jeita->bbc_psy = power_supply_get_by_name("charger");
	if (!nopmi_chg_jeita->bbc_psy) {
		pr_err("bbc supply not found, defer probe\n");
		return -EINVAL;
	}
	prop.intval = enabled;
	ret = power_supply_set_property(nopmi_chg_jeita->bbc_psy,
				POWER_SUPPLY_PROP_CHARGE_ENABLED, &prop);
	if (ret < 0) {
		pr_err("couldn't set voltage enabled, ret=%d\n", ret);
		return -EINVAL;
	}
	pr_err("2021.09.10 wsy end %s:enabled is %d\n", __func__, prop.intval);
	return 0;
}
*/
//add function for when charging and vbat > 4.1Vsuddenly temp > 480, batt ovp lead to reboot
/*static int batt_nopmi_chg_jeita_get_batt_voltage(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	union power_supply_propval prop = {0, };
	int ret = 0;
	int volt = 0;

    if(!nopmi_chg_jeita->bms_psy)
    {
    	nopmi_chg_jeita->bms_psy = power_supply_get_by_name("bms");
        if (!nopmi_chg_jeita->bms_psy) {
        	pr_err("bms supply not found, defer probe\n");
        	return -EINVAL;
        }
	}
	ret = power_supply_get_property(nopmi_chg_jeita->bms_psy,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &prop);
	if (ret < 0) {
		pr_err("couldn't read voltage property, ret=%d\n", ret);
		return -EINVAL;
	}
	volt = prop.intval/1000;
	pr_info("get_bat_voltage is %d\n", volt);
	return volt;
}*/
static int count_timer_ovp = 0;
static int count_timer_jeita_current = 0;
static void nopmi_chg_jeita_prevent_battery_ovp(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int batt_voltage = 0;
	struct sw_jeita_data *sw_jeita = nopmi_chg_jeita->sw_jeita;

	//batt_voltage = batt_nopmi_chg_jeita_get_batt_voltage(nopmi_chg_jeita);//vbat
	batt_voltage = nopmi_chg_jeita_get_battery_voltage(nopmi_chg_jeita);//cpvbatt
	if(batt_voltage >= (nopmi_chg_jeita->dt.jeita_temp_t3_to_t4_cv)){ //batt_voltage >= (4.2)V
		if (count_timer_ovp++ > 5){
			sw_jeita->charging = false;
			//sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
			vote(nopmi_chg_jeita->fcc_votable, JEITA_VOTER, true, 0);
			count_timer_ovp = 0;
			pr_err("sw_jeita->cv=%d sw_jeita->charging=%d  ovp cpvbatt=%d count_timer_ovp=%d\n",sw_jeita->cv,sw_jeita->charging,batt_voltage,count_timer_ovp);
		}
	} else { //batt_voltage < (4.1 + 0.01)V
		sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_t3_to_t4_cv;
		count_timer_ovp = 0;
	}
}

static void nopmi_chg_handle_jeita_current(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int ret = 0;
	int chg1_cv = 0;
	int term_curr_pre = 0;
	int pd_active = 0;
	int afc_active = 0;
	int pd_verified = 0;
	int cpvbatt = 0;
	int ibat = 0;
	struct sw_jeita_data *sw_jeita = nopmi_chg_jeita->sw_jeita;
	//union power_supply_propval prop = {0, };
	static int fast_charge_mode = 0;
	int fcc_vote_val =0;
	int cp_charging_enabled = 0; //this flag used by jeta:nopmi_chg_jeita.c to set sw_chip fv && used by fg_chip do another soc
	//struct nopmi_chg *chg = container_of(nopmi_chg_jeita, struct nopmi_chg, jeita_ctl);
	int vsm, pre_vsm;

	cpvbatt = nopmi_chg_jeita_get_battery_voltage(nopmi_chg_jeita);
	ibat = nopmi_chg_jeita_get_battery_current(nopmi_chg_jeita);
	//pr_info("enter.\n");
	sw_jeita->pre_sm = sw_jeita->sm;
	sw_jeita->charging = true;
	fcc_vote_val = get_effective_result(nopmi_chg_jeita->fcc_votable);
	/* JEITA battery temp Standard */
	pr_err("jeita fcc_vote_val=%d lcd_status=%d battery_temp=%d vbat=%d ibat=%d pre_sm=%d vsm=%d",fcc_vote_val,g_nopmi_chg->lcd_status, nopmi_chg_jeita->battery_temp, cpvbatt, ibat, sw_jeita->pre_sm, sw_jeita->vsm);
	if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t4_thres) {
		pr_info("[SW_JEITA] Battery Temperature(%d) Battery Over high Temperature(%d) !!\n",
			nopmi_chg_jeita->battery_temp,
			nopmi_chg_jeita->dt.temp_t4_thres);
		sw_jeita->sm = TEMP_ABOVE_T4;
		sw_jeita->charging = false;
		nopmi_chg_jeita->jeita_current_limit = 0;
	} else if (nopmi_chg_jeita->battery_temp > nopmi_chg_jeita->dt.temp_t3_thres) {
		/* control 45 degree to normal behavior */
		if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t4_thres_minus_x_degree) {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d,not allow charging yet!!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t4_thres_minus_x_degree,
				nopmi_chg_jeita->dt.temp_t4_thres);
			sw_jeita->charging = false;
		} else {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t3_thres,
				nopmi_chg_jeita->dt.temp_t4_thres);
			sw_jeita->sm = TEMP_T3_TO_T4;
			if((cpvbatt <= nopmi_chg_jeita->dt.jeita_temp_t3_to_t4_cv) || (sw_jeita->pre_sm == TEMP_ABOVE_T4)) {
				nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_t3_to_t4_fcc;
				count_timer_jeita_current = 0;
			} else {
				if ((count_timer_jeita_current++ > 5) && (!g_nopmi_chg->lcd_status)) {
					nopmi_chg_jeita->jeita_current_limit = 0;
					count_timer_jeita_current = 0;
				}
			}
			pr_err("45~55 sw_jeita->pre_sm =%d nopmi_chg_jeita->jeita_current_limit=%d cpvbatt=%d fcc_vote_val=%d count_timer_jeita_current=%d\n",sw_jeita->pre_sm,nopmi_chg_jeita->jeita_current_limit,cpvbatt,fcc_vote_val,count_timer_jeita_current);
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t2_thres) {
		if (((sw_jeita->sm == TEMP_T3_TO_T4)
		     && (nopmi_chg_jeita->battery_temp
			 >= nopmi_chg_jeita->dt.temp_t3_thres_minus_x_degree))
		    || ((sw_jeita->sm == TEMP_T1P5_TO_T2)
			&& (nopmi_chg_jeita->battery_temp
			    <= nopmi_chg_jeita->dt.temp_t2_thres_plus_x_degree))) {
			pr_info("[SW_JEITA] Battery Temperature not recovery to normal temperature charging mode yet!!\n");
		} else {
			pr_info("[SW_JEITA] Battery Normal Temperature(%d) between %d and %d !!\n",
		                nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t2_thres,
				nopmi_chg_jeita->dt.temp_t3_thres);
			sw_jeita->sm = TEMP_T2_TO_T3;
			if(sw_jeita->pre_sm != sw_jeita->sm) {
				sw_jeita->vsm = BATT_VOLTAGE;
			}
			pre_vsm = sw_jeita->vsm;
		#ifdef BATT_VOL_USE_CP
			if(cpvbatt > nopmi_chg_jeita->dt.jeita_temp_t2_to_t3_cv - JEITA_STEP_VOLTAGE_HYSTERISIS_MV){ //> 4280
		#else
			if(cpvbatt > 4315){ //> 4280, When the battery voltage is 4.315V, the cell voltage is less than 4.28V
		#endif
				vsm = BATT_VOLTAGE_4P28_ABOVE;
				sw_jeita->vsm = vsm;
				nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_t3_plus_fcc; //3800
			} else if (cpvbatt > nopmi_chg_jeita->dt.jeita_temp_t2_to_t3_p_cv - JEITA_STEP_VOLTAGE_HYSTERISIS_MV){ //> 4160
				vsm = BATT_VOLTAGE_4P16_TO_4P25;
				if (vsm < pre_vsm) {
					pr_err("vsm can not back to %d, stay %d\n", vsm, pre_vsm);
				} else {
					sw_jeita->vsm = vsm;
					nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_t2_to_t3_fcc; //4800
				}
			} else{
				vsm = BATT_VOLTAGE_3P5_TO_4P16;
				if (vsm < pre_vsm) {
					pr_err("vsm can not back to %d, stay %d\n", vsm, pre_vsm);
				} else {
					sw_jeita->vsm = vsm;
					nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_t2_to_t3_p_fcc; //5500
				}
			}
			pr_err("15 ~ 45 nopmi_chg_jeita->jeita_current_limit=%d cpvbatt=%d, pre_vsm=%d, vsm=%d sw_jeita->vsm=%d\n",nopmi_chg_jeita->jeita_current_limit,cpvbatt, pre_vsm, vsm, sw_jeita->vsm);
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t1p5_thres) {
		if ((sw_jeita->sm == TEMP_T1_TO_T1P5
		     || sw_jeita->sm == TEMP_T0_TO_T1)
		    && (nopmi_chg_jeita->battery_temp
			<= nopmi_chg_jeita->dt.temp_t1p5_thres_plus_x_degree)) {
			if (sw_jeita->sm == TEMP_T1_TO_T1P5) {
				pr_info("[SW_JEITA] Battery Normal Temperatur(%d) between %d and %d !!\n",
					nopmi_chg_jeita->battery_temp,
					nopmi_chg_jeita->dt.temp_t1p5_thres_plus_x_degree,
					nopmi_chg_jeita->dt.temp_t2_thres);
			}
			if (sw_jeita->sm == TEMP_T0_TO_T1) {
				pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
					nopmi_chg_jeita->battery_temp,
					nopmi_chg_jeita->dt.temp_t1_thres_plus_x_degree,
					nopmi_chg_jeita->dt.temp_t1p5_thres);
			}
			if (sw_jeita->sm == TEMP_TN1_TO_T0) {
				pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
				 nopmi_chg_jeita->battery_temp,
					nopmi_chg_jeita->dt.temp_t0_thres_plus_x_degree,
					nopmi_chg_jeita->dt.temp_tn1_thres);
			}
		} else {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t1p5_thres,
				nopmi_chg_jeita->dt.temp_t2_thres);
			sw_jeita->sm = TEMP_T1P5_TO_T2;
			if(cpvbatt > nopmi_chg_jeita->dt.jeita_temp_t3_to_t4_cv){
				nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_t3_plus_fcc;
			} else {
				nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_t1p5_to_t2_fcc;
			}
			pr_err("12 ~ 15 nopmi_chg_jeita->jeita_current_limit=%d cpvbatt=%d\n",nopmi_chg_jeita->jeita_current_limit,cpvbatt);
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t1_thres) {
		if ((sw_jeita->sm == TEMP_T0_TO_T1
			|| sw_jeita->sm == TEMP_BELOW_T0
			|| sw_jeita->sm == TEMP_TN1_TO_T0)
			&& (nopmi_chg_jeita->battery_temp
			<= nopmi_chg_jeita->dt.temp_t1_thres_plus_x_degree)) {
		    if (sw_jeita->sm == TEMP_T0_TO_T1) {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
				 nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t1_thres_plus_x_degree,
				nopmi_chg_jeita->dt.temp_t1p5_thres);
			}
			if (sw_jeita->sm == TEMP_BELOW_T0) {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d,not allow charging yet!!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_tn1_thres,
				nopmi_chg_jeita->dt.temp_tn1_thres_plus_x_degree);
				sw_jeita->charging = false;
			}
		} else {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t1_thres,
				nopmi_chg_jeita->dt.temp_t1p5_thres);
			sw_jeita->sm = TEMP_T1_TO_T1P5;
			nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_t1_to_t1p5_fcc;
		}
	} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t0_thres) {
		if ((sw_jeita->sm == TEMP_BELOW_T0
			|| sw_jeita->sm == TEMP_TN1_TO_T0)
			&& (nopmi_chg_jeita->battery_temp
			<= nopmi_chg_jeita->dt.temp_t0_thres_plus_x_degree)) {
			if (sw_jeita->sm == TEMP_BELOW_T0) {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d,not allow charging yet!!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_tn1_thres,
				nopmi_chg_jeita->dt.temp_tn1_thres_plus_x_degree);
				sw_jeita->charging = false;
			} else if (sw_jeita->sm == TEMP_TN1_TO_T0) {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t0_thres_plus_x_degree,
				nopmi_chg_jeita->dt.temp_tn1_thres);
			}
		} else {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t0_thres,
				nopmi_chg_jeita->dt.temp_t1_thres);
			sw_jeita->sm = TEMP_T0_TO_T1;
			nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_t0_to_t1_fcc;
		}
	/*} else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_tn1_thres) {
		if ((sw_jeita->sm == TEMP_BELOW_T0)
			&& (nopmi_chg_jeita->battery_temp
			<= nopmi_chg_jeita->dt.temp_tn1_thres_plus_x_degree)) {
			pr_info("[SW_JEITA] 1Battery Temperature(%d) between %d and %d,not allow charging yet!!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_tn1_thres,
				nopmi_chg_jeita->dt.temp_tn1_thres_plus_x_degree);
			sw_jeita->charging = false;
		} else {
			pr_info("[SW_JEITA] Battery Temperature(%d) between %d and %d !!\n",
				nopmi_chg_jeita->battery_temp,
				nopmi_chg_jeita->dt.temp_t0_thres,
				nopmi_chg_jeita->dt.temp_tn1_thres);
			sw_jeita->sm = TEMP_TN1_TO_T0;
			nopmi_chg_jeita->jeita_current_limit = nopmi_chg_jeita->dt.temp_tn1_to_t0_fcc;
		}*/
	} else {
		pr_info("[SW_JEITA]Battery Temperature(%d) Battery below low Temperature(%d) !!\n",
		        nopmi_chg_jeita->battery_temp,
			    nopmi_chg_jeita->dt.temp_t0_thres);
		sw_jeita->sm = TEMP_BELOW_T0;
		sw_jeita->charging = false;
		nopmi_chg_jeita->jeita_current_limit = 0;
	}
/*
    ret = nopmi_chg_get_iio_channel(chg, NOPMI_BMS, FG_FASTCHARGE_MODE, &fast_charge_mode);
	if (ret != 0) {
        pr_err("couldn't get fastcharge mode property, ret=%d\n", ret);
        return;
	}
	if(!g_ffc_disable && fast_charge_mode && (sw_jeita->sm != TEMP_T2_TO_T3)){
		prop.intval = 0;
		fast_charge_mode = 0;
		ret = nopmi_chg_set_iio_channel(chg, NOPMI_BMS, FG_FASTCHARGE_MODE, fast_charge_mode);
        if (ret != 0) {
            pr_err("couldn't set fastcharge mode property, ret=%d\n", ret);
            return;
	    }
	}else if(!g_ffc_disable && !fast_charge_mode && (sw_jeita->sm == TEMP_T2_TO_T3)){
		prop.intval = 1;
		fast_charge_mode = 1;
		ret = nopmi_chg_set_iio_channel(chg, NOPMI_BMS, FG_FASTCHARGE_MODE, fast_charge_mode);
         if (ret != 0) {
            pr_err("couldn't set fastcharge mode property, ret=%d\n", ret);
            return;
	    }
	}*/
    /* add for update fastcharge mode, end*/
	//pd_verified = adapter_dev_get_pd_verified();
	/* set CV after temperature changed */
	/* In normal range, we adjust CV dynamically */
	if (sw_jeita->sm != TEMP_T2_TO_T3) {
		if (sw_jeita->sm == TEMP_ABOVE_T4){
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_above_t4_cv;
			nopmi_chg_jeita_prevent_battery_ovp(nopmi_chg_jeita);
		}else if (sw_jeita->sm == TEMP_T3_TO_T4){
			sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_t3_to_t4_cv;
			nopmi_chg_jeita_prevent_battery_ovp(nopmi_chg_jeita);
		}else if (sw_jeita->sm == TEMP_T2_TO_T3)
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
		else if (sw_jeita->sm == TEMP_T1P5_TO_T2)
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
		else if (sw_jeita->sm == TEMP_T1_TO_T1P5)
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
		else if (sw_jeita->sm == TEMP_T0_TO_T1)
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
		else if (sw_jeita->sm == TEMP_TN1_TO_T0)
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
		else if (sw_jeita->sm == TEMP_BELOW_T0)
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
		else
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
	} else {
			sw_jeita->cv = nopmi_chg_jeita->dt.normal_charge_voltage;
			/*Both temp is normal and FFC is enabled, then improve FV*/
			if(fast_charge_mode && !g_ffc_disable){
				if(NOPMI_CHARGER_IC_MAXIM == nopmi_get_charger_ic_type()){
					sw_jeita->cv = 4470; //for maxim chip
				}else{
					sw_jeita->cv = 4470; //for other pmic chips
				}
				if(!pd_verified){
					sw_jeita->cv = 4470; //for unverified pd
				}
			}
	}
	if(cp_charging_enabled) {
		if(NOPMI_CHARGER_IC_MAXIM != nopmi_get_charger_ic_type()) {
			pr_info("charge pump :sw_jeita->cv = 4608.\n");
			sw_jeita->cv = 4350; //if charger pump working set sw_chip fv:4608
		}
	}
	if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t4_plus_thres) {

		sw_jeita->cv = nopmi_chg_jeita->dt.jeita_temp_t3_to_t4_cv;
		pr_err("[sw-jeita] Battery Temperature(%d) Battery temp_t4_plus_thres(%d) sw_jeita->cv=%d nopmi_chg_jeita->jeita_current_limit=%d!!\n",
			nopmi_chg_jeita->battery_temp,
			nopmi_chg_jeita->dt.temp_t4_plus_thres,sw_jeita->cv,nopmi_chg_jeita->jeita_current_limit);
	}
	pr_err("[sw-jeita] write before sw_jeita->cv=%d nopmi_chg_jeita->jeita_current_limit=%d sm=%d!!\n",sw_jeita->cv,nopmi_chg_jeita->jeita_current_limit, sw_jeita->sm);
	if(nopmi_chg_jeita->fcc_votable)
	{
		if(TEMP_T2_TO_T3 == sw_jeita->sm) {
			if (vsm < pre_vsm) {
				pr_err("Voltage drops to the previous gear. pre_vsm=%d,vsm=%d,sw_jeita->vsm=%d", pre_vsm, vsm, sw_jeita->vsm);
			} else
				vote(nopmi_chg_jeita->fcc_votable, JEITA_VOTER, true, max(nopmi_chg_jeita->jeita_current_limit, min(fcc_vote_val,ibat) - TAPERED_FCC_REDUCTION_STEP_MA));
		}
		else
			vote(nopmi_chg_jeita->fcc_votable, JEITA_VOTER, true, nopmi_chg_jeita->jeita_current_limit);
	} else {
		ret = nopmi_chg_jeita_set_charger_current(nopmi_chg_jeita, nopmi_chg_jeita->jeita_current_limit);
	}
	if(nopmi_chg_jeita->fv_votable) {
		chg1_cv =  get_effective_result(nopmi_chg_jeita->fv_votable);
	} else {
		chg1_cv = nopmi_chg_jeita_get_charger_voltage(nopmi_chg_jeita);
	}
	if (sw_jeita->cv != chg1_cv){
		if(nopmi_chg_jeita->fv_votable)
		{
			vote(nopmi_chg_jeita->fv_votable, JEITA_VOTER, true, sw_jeita->cv);
		} else {
			ret = nopmi_chg_jeita_set_charger_voltage(nopmi_chg_jeita, sw_jeita->cv);
			if (ret < 0)
				pr_err("Couldn't set cv to %d, rc:%d\n", sw_jeita->cv, ret);
		}
	}
	/* set term current after temperature changed */
	pd_active = nopmi_chg_jeita_get_pd_active(nopmi_chg_jeita);
	afc_active = nopmi_chg_jeita_get_afc_active(nopmi_chg_jeita);
	pr_info("get pd_active: %d  afc_active: %d\n", pd_active, afc_active);
	if ((pd_active == 1) || (afc_active == 1)) {
		sw_jeita->term_curr = MAIN_QUICK_CHARGE_ITERM;
	} else {
		sw_jeita->term_curr = MAIN_CHARGE_ITERM;
	}

	term_curr_pre = nopmi_chg_jeita_get_charger_term_current(nopmi_chg_jeita);
	if ((sw_jeita->term_curr > 0) && (sw_jeita->term_curr != term_curr_pre)) {
		ret = nopmi_chg_jeita_set_charger_term_current(nopmi_chg_jeita, sw_jeita->term_curr);
	if (ret < 0)
		pr_err("Couldn't set term curr to %d, rc:%d\n", sw_jeita->term_curr, ret);
	}

	pr_info("[SW_JEITA]preState:%d newState:%d temp:%d cv:%d,%d nopmi_chg_jeita->jeita_current_limit:%d, term_curr:%d,%d, batt_id:%d, pd_active:%d\n",
	sw_jeita->pre_sm, sw_jeita->sm, nopmi_chg_jeita->battery_temp,
	sw_jeita->cv, chg1_cv, nopmi_chg_jeita->jeita_current_limit, sw_jeita->term_curr,
	term_curr_pre, nopmi_chg_jeita->battery_id, pd_active);
	continue_nopmi_chg_jeita_workfunc();
}

static void nopmi_chg_handle_jeita(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int ret = 0;
	struct nopmi_chg *chg = container_of(nopmi_chg_jeita, struct nopmi_chg, jeita_ctl);

	nopmi_chg_jeita->battery_temp = nopmi_chg_jeita_get_bat_temperature(nopmi_chg_jeita);
	nopmi_chg_handle_jeita_current(nopmi_chg_jeita);
	if(nopmi_chg_jeita->sw_jeita->charging == false) {
		nopmi_chg_jeita->sw_jeita->can_recharging = true;
		ret = vote(chg->fcc_votable, JEITA_VOTER, true, 0);
		if(ret < 0){
			pr_err("%s : fcc vote failed \n", __func__);
		}
		/*switch(nopmi_get_charger_ic_type()) {
			case NOPMI_CHARGER_IC_MAXIM:
				vote(nopmi_chg_jeita->chgctrl_votable, JEITA_CHG_VOTER, true, CHG_MODE_CHARGING_OFF);
				break;
			case NOPMI_CHARGER_IC_SYV:
				//nopmi_chg_jeita_set_charger_enabled(nopmi_chg_jeita, false);
				//main_set_charge_enable(false);
				ret = nopmi_chg_set_iio_channel(chg, NOPMI_MAIN, MAIN_CHARGE_ENABLED, 0);
				pr_info("%s ret=%d\n", __func__, ret);
				break;
			case NOPMI_CHARGER_IC_SC:
				break;
			default:
				break;
		}*/
	} else {
		if(nopmi_chg_jeita->sw_jeita->can_recharging == true) {
			ret = vote(chg->fcc_votable, JEITA_VOTER, false, 0);
			if(ret < 0){
				pr_err("%s : fcc vote failed \n", __func__);
			}
			switch(nopmi_get_charger_ic_type()) {
				case NOPMI_CHARGER_IC_MAXIM:
					vote(nopmi_chg_jeita->chgctrl_votable, JEITA_CHG_VOTER, false, CHG_MODE_CHARGING_OFF);
					break;
				case NOPMI_CHARGER_IC_SYV:
					//nopmi_chg_jeita_set_charger_enabled(nopmi_chg_jeita, true);
					//main_set_charge_enable(true);
					ret = nopmi_chg_set_iio_channel(chg, NOPMI_MAIN, MAIN_CHARGE_ENABLED, 1);
					pr_info("%s ret=%d\n", __func__, ret);
					break;
				case NOPMI_CHARGER_IC_SC:
					break;
				default:
					break;
			}
			nopmi_chg_jeita->sw_jeita->can_recharging = false;
		}
	}
}

static void nopmi_chg_jeita_workfunc(struct work_struct *work)
{
	struct nopmi_chg_jeita_st *chg_jeita = container_of(work,
		struct nopmi_chg_jeita_st, jeita_work.work);

	//pr_info("enter.\n");
	if (!g_nopmi_chg->usb_online){
		pr_err("usb not present,return!\n");
		return;
	}


	/* skip elapsed_us debounce for handling battery temperature */
	if(chg_jeita->dt.enable_sw_jeita == true) {
		nopmi_chg_handle_jeita(chg_jeita);
		chg_jeita->sw_jeita_start = true;
	} else {
		chg_jeita->sw_jeita_start = false;
	}
}

void start_nopmi_chg_jeita_workfunc(void)
{
	if(g_nopmi_chg_jeita) {
		if(!g_nopmi_chg_jeita->jeita_work_start) {
			g_nopmi_chg_jeita->sw_jeita->vsm = BATT_VOLTAGE;
			schedule_delayed_work(&g_nopmi_chg_jeita->jeita_work,
				0);
		} else {
			schedule_delayed_work(&g_nopmi_chg_jeita->jeita_work,
				msecs_to_jiffies(JEITA_WORK_DELAY_MS));
		}
		g_nopmi_chg_jeita->jeita_work_start = true;
	}
}

static void continue_nopmi_chg_jeita_workfunc(void)
{
	if(g_nopmi_chg_jeita) {
		schedule_delayed_work(&g_nopmi_chg_jeita->jeita_work,
			msecs_to_jiffies(JEITA_WORK_DELAY_MS));
	}
}

void stop_nopmi_chg_jeita_workfunc(void)
{
	if(g_nopmi_chg_jeita) {
		cancel_delayed_work_sync(&g_nopmi_chg_jeita->jeita_work);
		if(g_nopmi_chg_jeita->dt.enable_sw_jeita == true) {
			/*JEITA_VOTER setting changed from false to true 480
			Prevent the current exceeding the jeita standard at the moment of next insertion*/
			vote(g_nopmi_chg_jeita->fcc_votable, JEITA_VOTER, true, JEITA_MIN_FCC);
		} else {
			vote(g_nopmi_chg_jeita->fcc_votable, JEITA_VOTER, false, 0);
		}
		g_nopmi_chg_jeita->jeita_work_start = false;
	}
}

static void nopmi_chg_jeita_state_init(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	struct sw_jeita_data *sw_jeita = nopmi_chg_jeita->sw_jeita;
	
	if (nopmi_chg_jeita->dt.enable_sw_jeita == true) {
		nopmi_chg_jeita->battery_temp = nopmi_chg_jeita_get_bat_temperature(nopmi_chg_jeita);
		if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t4_thres)
			sw_jeita->sm = TEMP_ABOVE_T4;
		else if (nopmi_chg_jeita->battery_temp > nopmi_chg_jeita->dt.temp_t3_thres)
			sw_jeita->sm = TEMP_T3_TO_T4;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t2_thres)
			sw_jeita->sm = TEMP_T2_TO_T3;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t1p5_thres)
			sw_jeita->sm = TEMP_T1P5_TO_T2;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t1_thres)
			sw_jeita->sm = TEMP_T1_TO_T1P5;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_t0_thres)
			sw_jeita->sm = TEMP_T0_TO_T1;
		else if (nopmi_chg_jeita->battery_temp >= nopmi_chg_jeita->dt.temp_tn1_thres)
			sw_jeita->sm = TEMP_TN1_TO_T0;
		else
			sw_jeita->sm = TEMP_BELOW_T0;
		
		pr_info("[SW_JEITA] tmp:%d sm:%d\n",
			nopmi_chg_jeita->battery_temp, sw_jeita->sm);
	}
}

int nopmi_chg_jeita_init(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int rc = 0;
	g_nopmi_chg_jeita = nopmi_chg_jeita;

	//pr_info("enter.\n");
	if (!nopmi_chg_jeita->sw_jeita ) {
		nopmi_chg_jeita->sw_jeita = kmalloc(sizeof(struct sw_jeita_data), GFP_KERNEL);
		if(!nopmi_chg_jeita->sw_jeita)
		{
			pr_err(" nopmi_chg_jeita_init Failed to allocate memory\n");
			return -ENOMEM;
		}
	}
	nopmi_chg_jeita->fcc_votable = find_votable("FCC");
	nopmi_chg_jeita->fv_votable = find_votable("FV");
	nopmi_chg_jeita->usb_icl_votable = find_votable("USB_ICL");
	nopmi_chg_jeita->chgctrl_votable = find_votable("CHG_CTRL");
	nopmi_chg_jeita_state_init(nopmi_chg_jeita);
	if(NOPMI_CHARGER_IC_MAXIM != nopmi_get_charger_ic_type()){
		nopmi_chg_jeita->battery_id = nopmi_chg_jeita_get_batt_id(nopmi_chg_jeita);
	}
	INIT_DELAYED_WORK(&nopmi_chg_jeita->jeita_work, nopmi_chg_jeita_workfunc);
	nopmi_chg_jeita->jeita_work_start = false;
	return rc;
}

int nopmi_chg_jeita_deinit(struct nopmi_chg_jeita_st *nopmi_chg_jeita)
{
	int rc = 0;
	g_nopmi_chg_jeita = NULL;

	//pr_info("enter.\n");
	if (!nopmi_chg_jeita->sw_jeita ) {
		cancel_delayed_work_sync(&nopmi_chg_jeita->jeita_work);
		kfree(nopmi_chg_jeita->sw_jeita);
	}
	return rc;
}

MODULE_DESCRIPTION("Charger Driver");
MODULE_LICENSE("GPL v2");