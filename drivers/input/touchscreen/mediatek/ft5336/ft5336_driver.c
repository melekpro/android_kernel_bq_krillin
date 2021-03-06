/* 
******************************************************************************
* @file    ft5336.c                                                          *
* @author  MCD Application Team                                              *
* @version V1.0.0                                                            *		
* @date    25-June-2015                                                      *		
* @brief   This file contains all the functions prototypes for the           *		
*          ft5336.c Touch screen driver.                                     *		
******************************************************************************
*/
#include "tpd.h"
#include <linux/interrupt.h>
#include <cust_eint.h>
#include <linux/i2c.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/rtpm_prio.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>

#include "tpd_custom_ft5336.h"
#include <mach/mt_pm_ldo.h>
#include <mach/mt_typedefs.h>
#include <mach/mt_boot.h>

#include <cust_vibrator.h>
#include "cust_gpio_usage.h"

#ifdef FTS_CTL_IIC
#include "focaltech_ctl.h"
#endif


#define TPD_MAX_PONIT       5 
extern void custom_vibration_enable(int);
extern kal_bool check_charger_exist(void); 

extern struct tpd_device *tpd;
 
struct i2c_client *ft5336_i2c_client = NULL;
struct task_struct *ft5336_thread = NULL;
 
static DECLARE_WAIT_QUEUE_HEAD(waiter);
 
 
static void tpd_eint_interrupt_handler(void);

#if 0
extern void mt65xx_eint_unmask(unsigned int line);
extern void mt65xx_eint_mask(unsigned int line);
extern void mt65xx_eint_set_hw_debounce(unsigned int eint_num, unsigned int ms);
extern unsigned int mt65xx_eint_set_sens(unsigned int eint_num, unsigned int sens);
extern void mt65xx_eint_registration(unsigned int eint_num, unsigned int is_deb_en, unsigned int pol, void (EINT_FUNC_PTR)(void), unsigned int is_auto_umask);
#endif
 
static int tpd_probe(struct i2c_client *client, const struct i2c_device_id *id);
static int tpd_detect (struct i2c_client *client, struct i2c_board_info *info);
static int tpd_remove(struct i2c_client *client);
static int touch_event_handler(void *unused);
 
static int boot_mode = 0;
static int tpd_halt=0; 
static int tpd_flag = 0;
static int point_num = 0;
static int p_point_num = 0;
static bool discard_resume_first_eint = KAL_FALSE;
static int tpd_state = 0;

#define TPD_OK 0

#define DEVICE_MODE 0x00
#define GEST_ID 0x01
#define TD_STATUS 0x02

#define TOUCH1_XH 0x03
#define TOUCH1_XL 0x04
#define TOUCH1_YH 0x05
#define TOUCH1_YL 0x06

#define TOUCH2_XH 0x09
#define TOUCH2_XL 0x0A
#define TOUCH2_YH 0x0B
#define TOUCH2_YL 0x0C

#define TOUCH3_XH 0x0F
#define TOUCH3_XL 0x10
#define TOUCH3_YH 0x11
#define TOUCH3_YL 0x12

#define CONFIG_SUPPORT_FTS_CTP_UPG

#define TPD_RESET_ISSUE_WORKAROUND

#define TPD_MAX_RESET_COUNT 3


#ifdef ESD_CHECK
static struct delayed_work ctp_read_id_work;
static struct workqueue_struct * ctp_read_id_workqueue = NULL;
#endif

#ifdef TPD_HAVE_BUTTON 
static int tpd_keys_local[TPD_KEY_COUNT] = TPD_KEYS;
static int tpd_keys_dim_local[TPD_KEY_COUNT][4] = TPD_KEYS_DIM;
#endif
#if (defined(TPD_WARP_START) && defined(TPD_WARP_END))
static int tpd_wb_start_local[TPD_WARP_CNT] = TPD_WARP_START;
static int tpd_wb_end_local[TPD_WARP_CNT]   = TPD_WARP_END;
#endif
#if (defined(TPD_HAVE_CALIBRATION) && !defined(TPD_CUSTOM_CALIBRATION))
static int tpd_calmat_local[8]     = TPD_CALIBRATION_MATRIX;
static int tpd_def_calmat_local[8] = TPD_CALIBRATION_MATRIX;
#endif

#define VELOCITY_CUSTOM_FT5206
#ifdef VELOCITY_CUSTOM_FT5206
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>

// for magnify velocity
#ifndef TPD_VELOCITY_CUSTOM_X
#define TPD_VELOCITY_CUSTOM_X 10
#endif
#ifndef TPD_VELOCITY_CUSTOM_Y
#define TPD_VELOCITY_CUSTOM_Y 10
#endif

#define TOUCH_IOC_MAGIC 'A'

#define TPD_GET_VELOCITY_CUSTOM_X _IO(TOUCH_IOC_MAGIC,0)
#define TPD_GET_VELOCITY_CUSTOM_Y _IO(TOUCH_IOC_MAGIC,1)
#define TPD_GET_ENABLE_GESTRUE _IO(TOUCH_IOC_MAGIC,3)
#define TPD_SET_ENABLE_GESTRUE _IO(TOUCH_IOC_MAGIC,4)

#if defined (CONFIG_SUPPORT_FTS_CTP_UPG)
#define TPD_UPGRADE_CKT _IO(TOUCH_IOC_MAGIC,2)
static unsigned char CtpFwUpgradeForIOCTRL(unsigned char* pbt_buf, unsigned int dw_lenth);
static DEFINE_MUTEX(fwupgrade_mutex);
atomic_t    upgrading;
#endif

int g_v_magnify_x =TPD_VELOCITY_CUSTOM_X;
int g_v_magnify_y =TPD_VELOCITY_CUSTOM_Y;
static int tpd_misc_open(struct inode *inode, struct file *file)
{
	return nonseekable_open(inode, file);
}
/*----------------------------------------------------------------------------*/
static int tpd_misc_release(struct inode *inode, struct file *file)
{
	return 0;
}
/*----------------------------------------------------------------------------*/
static long tpd_unlocked_ioctl(struct file *file, unsigned int cmd,
       unsigned long arg)
{
	void __user *data;
	
	long err = 0;
	int size=0;
	char * ctpdata=NULL;

	if(_IOC_DIR(cmd) & _IOC_READ)
	{
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	}
	else if(_IOC_DIR(cmd) & _IOC_WRITE)
	{
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	}

	if(err)
	{
		// Delete logspam on Touch Driver Pablito2020
		//printk("tpd: access error: %08X, (%2d, %2d)\n", cmd, _IOC_DIR(cmd), _IOC_SIZE(cmd));
		return -EFAULT;
	}

	switch(cmd)
	{
		case TPD_GET_VELOCITY_CUSTOM_X:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}			
			
			if(copy_to_user(data, &g_v_magnify_x, sizeof(g_v_magnify_x)))
			{
				err = -EFAULT;
				break;
			}				 
			break;

	   case TPD_GET_VELOCITY_CUSTOM_Y:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}			
			
			if(copy_to_user(data, &g_v_magnify_y, sizeof(g_v_magnify_y)))
			{
				err = -EFAULT;
				break;
			}				 
			break;

#if defined (CONFIG_SUPPORT_FTS_CTP_UPG)
		case TPD_UPGRADE_CKT:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}	
			if(copy_from_user(&size, data, sizeof(int)))
			{
				err = -EFAULT;
				break;	  
			}
			ctpdata=kmalloc(size, GFP_KERNEL);
			if(ctpdata==NULL)
			{
				err = -EFAULT;
				break;
			}
			
			if(copy_from_user(ctpdata, data+sizeof(int), size))
			{
				kfree(ctpdata);
				err = -EFAULT;
				break;	  
			}
			err=CtpFwUpgradeForIOCTRL(ctpdata, size);
 			kfree(ctpdata);
			break;
#endif
		default:
			//Delete logspam on touchscreen driver, Pablito2020
			//printk("tpd: unknown IOCTL: 0x%08x\n", cmd);
			err = -ENOIOCTLCMD;
			break;
			
	}

	return err;
}


static struct file_operations tpd_fops = {
	.open = tpd_misc_open,
	.release = tpd_misc_release,
	.unlocked_ioctl = tpd_unlocked_ioctl,
};
/*----------------------------------------------------------------------------*/
static struct miscdevice tpd_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ft5336",
	.fops = &tpd_fops,
};

//**********************************************
#endif

struct touch_info {
    int y[5];
    int x[5];
    int p[5];
	int id[5];
    int count;
    #ifdef FTS_PRESSURE
    int au8_touch_weight[TPD_MAX_PONIT];/*touch weight*/
    int au8_touch_area[TPD_MAX_PONIT];/*touch area*/
    #endif
};
 
 static const struct i2c_device_id ft5336_tpd_id[] = {{"ft5336",0},{}};
 static struct i2c_board_info __initdata ft5336_i2c_tpd={ I2C_BOARD_INFO("ft5336", (0x70>>1))};
 
 
 static struct i2c_driver tpd_i2c_driver = {
  .driver = {
	 .name = "ft5336",
  },
  .probe = tpd_probe,
  .remove = tpd_remove,
  .id_table = ft5336_tpd_id,
  .detect = tpd_detect,
 };
 #ifdef CONFIG_SUPPORT_FTS_CTP_UPG
static u8 *CTPI2CDMABuf_va = NULL;
static u32 CTPI2CDMABuf_pa = NULL;
typedef enum
{
    ERR_OK,
    ERR_MODE,
    ERR_READID,
    ERR_ERASE,
    ERR_STATUS,
    ERR_ECC,
    ERR_DL_ERASE_FAIL,
    ERR_DL_PROGRAM_FAIL,
    ERR_DL_VERIFY_FAIL
}E_UPGRADE_ERR_TYPE;

typedef unsigned char         FTS_BYTE;     //8 bit
typedef unsigned short        FTS_WORD;    //16 bit
typedef unsigned int          FTS_DWRD;    //16 bit
typedef unsigned char         FTS_BOOL;    //8 bit

#define FTS_NULL                0x0
#define FTS_TRUE                0x01
#define FTS_FALSE              0x0

#define I2C_CTPM_ADDRESS       0x70

/***********************************************************************************************
Name	:	ft5x0x_i2c_rxdata 
Input	:	*rxdata
                     *length
Output	:	ret
function	:	
***********************************************************************************************/
static int ft5x0x_i2c_rxdata(char *rxdata, int length)
{
	int ret;

	struct i2c_msg msgs[] = {
		{
			.addr	= ft5336_i2c_client->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= rxdata,
		},
		{
			.addr	= ft5336_i2c_client->addr,
			.flags	= I2C_M_RD,
			.len	= length,
			.buf	= rxdata,
		},
	};

    //msleep(1);
	ret = i2c_transfer(ft5336_i2c_client->adapter, msgs, 2);
	if (ret < 0)
		pr_err("msg %s i2c read error: %d\n", __func__, ret);
	
	return ret;
}
/***********************************************************************************************
Name	:	 
Input	:	
Output	:	
function	:	
***********************************************************************************************/
static int ft5x0x_i2c_txdata(char *txdata, int length)
{
	int ret;

	struct i2c_msg msg[] = {
		{
			.addr	= ft5336_i2c_client->addr,
			.flags	= 0,
			.len	= length,
			.buf	= txdata,
		},
	};

   	//msleep(1);
	ret = i2c_transfer(ft5336_i2c_client->adapter, msg, 1);
	if (ret < 0)
		pr_err("%s i2c write error: %d\n", __func__, ret);

	return ret;
}
/***********************************************************************************************
Name	:	 ft5x0x_write_reg
Input	:	addr -- address
                     para -- parameter
Output	:	
function	:	write register of ft5x0x
***********************************************************************************************/
static int ft5x0x_write_reg(u8 addr, u8 para)
{
    u8 buf[3];
    int ret = -1;

    buf[0] = addr;
    buf[1] = para;
    ret = ft5x0x_i2c_txdata(buf, 2);
    if (ret < 0) {
        pr_err("write reg failed! %#x ret: %d", buf[0], ret);
        return -1;
    }
  
    return 0;
}

/***********************************************************************************************
Name	:	ft5x0x_read_reg 
Input	:	addr
                     pdata
Output	:	
function	:	read register of ft5x0x
***********************************************************************************************/
static int ft5x0x_read_reg(u8 addr, u8 *pdata)
{
	int ret;
	u8 buf[2] = {0};

	buf[0] = addr;
	struct i2c_msg msgs[] = {
		{
			.addr	= ft5336_i2c_client->addr,
			.flags	= 0,
			.len	= 1,
			.buf	= buf,
		},
		{
			.addr	= ft5336_i2c_client->addr,
			.flags	= I2C_M_RD,
			.len	= 1,
			.buf	= buf,
		},
	};

    //msleep(1);
	ret = i2c_transfer(ft5336_i2c_client->adapter, msgs, 2);
	if (ret < 0)
		pr_err("msg %s i2c read error: %d\n", __func__, ret);

	*pdata = buf[0];
	return ret;
  
}

/***********************************************************************************************
Name	:	 ft5x0x_read_fw_ver

Input	:	 void
                     

Output	:	 firmware version 	

function	:	 read TP firmware version

***********************************************************************************************/
static unsigned char ft5x0x_read_fw_ver(void)
{
	unsigned char ver;
	ft5x0x_read_reg(0xa6, &ver);
	return(ver);
}
static unsigned char ft5x0x_read_ID_ver(void)
{
	unsigned char ver;
	ft5x0x_read_reg(0xa8, &ver);
	return(ver);
}

static unsigned char ft5x0x_read_doubleclick_flag(void)
{
	unsigned char ver;
	ft5x0x_read_reg(0xcc, &ver);
	return(ver==0xaa?1:0);
}

static void delay_qt_ms(unsigned long  w_ms)
{
    unsigned long i;
    unsigned long j;

    for (i = 0; i < w_ms; i++)
    {
        for (j = 0; j < 1000; j++)
        {
            udelay(1);
        }
    }
}

/*
[function]: 
    callback: read data from ctpm by i2c interface,implemented by special user;
[parameters]:
    bt_ctpm_addr[in]    :the address of the ctpm;
    pbt_buf[out]        :data buffer;
    dw_lenth[in]        :the length of the data buffer;
[return]:
    FTS_TRUE     :success;
    FTS_FALSE    :fail;
*/
FTS_BOOL i2c_read_interface(FTS_BYTE bt_ctpm_addr, FTS_BYTE* pbt_buf, FTS_DWRD dw_lenth)
{
    int ret;
    
    ret=i2c_master_recv(ft5336_i2c_client, pbt_buf, dw_lenth);

    if(ret<=0)
    {
	// Disable touchscreen logspam, Pablito2020
        //printk("[TSP]i2c_read_interface error\n");
        return FTS_FALSE;
    }
  
    return FTS_TRUE;
}

/*
[function]: 
    callback: write data to ctpm by i2c interface,implemented by special user;
[parameters]:
    bt_ctpm_addr[in]    :the address of the ctpm;
    pbt_buf[in]        :data buffer;
    dw_lenth[in]        :the length of the data buffer;
[return]:
    FTS_TRUE     :success;
    FTS_FALSE    :fail;
*/
FTS_BOOL i2c_write_interface(FTS_BYTE bt_ctpm_addr, FTS_BYTE* pbt_buf, FTS_DWRD dw_lenth)
{
    int ret;
    ret=i2c_master_send(ft5336_i2c_client, pbt_buf, dw_lenth);
    if(ret<=0)
    {
	    // Disable touchscreen logspam, Pablito2020
        //printk("[TSP]i2c_write_interface error line = %d, ret = %d\n", __LINE__, ret);
        return FTS_FALSE;
    }

    return FTS_TRUE;
}

/*
[function]: 
    send a command to ctpm.
[parameters]:
    btcmd[in]        :command code;
    btPara1[in]    :parameter 1;    
    btPara2[in]    :parameter 2;    
    btPara3[in]    :parameter 3;    
    num[in]        :the valid input parameter numbers, if only command code needed and no parameters followed,then the num is 1;    
[return]:
    FTS_TRUE    :success;
    FTS_FALSE    :io fail;
*/
FTS_BOOL cmd_write(FTS_BYTE btcmd,FTS_BYTE btPara1,FTS_BYTE btPara2,FTS_BYTE btPara3,FTS_BYTE num)
{
    FTS_BYTE write_cmd[4] = {0};

    write_cmd[0] = btcmd;
    write_cmd[1] = btPara1;
    write_cmd[2] = btPara2;
    write_cmd[3] = btPara3;
    return i2c_write_interface(I2C_CTPM_ADDRESS, write_cmd, num);
}

/*
[function]: 
    write data to ctpm , the destination address is 0.
[parameters]:
    pbt_buf[in]    :point to data buffer;
    bt_len[in]        :the data numbers;    
[return]:
    FTS_TRUE    :success;
    FTS_FALSE    :io fail;
*/
FTS_BOOL byte_write(FTS_BYTE* pbt_buf, FTS_DWRD dw_len)
{
    
    return i2c_write_interface(I2C_CTPM_ADDRESS, pbt_buf, dw_len);
}

static int CTPDMA_i2c_write(FTS_BYTE slave,FTS_BYTE* pbt_buf, FTS_DWRD dw_len)
{
    
	int i = 0;
	int err = 0;
	for(i = 0 ; i < dw_len; i++)
	{
		CTPI2CDMABuf_va[i] = pbt_buf[i];
	}

	if(dw_len <= 8)
	{
		//i2c_client->addr = i2c_client->addr & I2C_MASK_FLAG;
		//MSE_ERR("Sensor non-dma write timing is %x!\r\n", this_client->timing);
		return i2c_master_send(ft5336_i2c_client, pbt_buf, dw_len);
	}
	else
	{
		ft5336_i2c_client->addr = ft5336_i2c_client->addr & I2C_MASK_FLAG | I2C_DMA_FLAG;
		//MSE_ERR("Sensor dma timing is %x!\r\n", this_client->timing);
		err= i2c_master_send(ft5336_i2c_client, CTPI2CDMABuf_pa, dw_len);
		ft5336_i2c_client->addr = ft5336_i2c_client->addr & I2C_MASK_FLAG;
		return err;
	}    
}


static int CTPDMA_i2c_read(FTS_BYTE slave, FTS_BYTE *buf, FTS_DWRD len)
{
	int i = 0, err = 0;

	if(len < 8)
	{
		ft5336_i2c_client->addr = ft5336_i2c_client->addr & I2C_MASK_FLAG;
		//MSE_ERR("Sensor non-dma read timing is %x!\r\n", this_client->timing);
		return i2c_master_recv(ft5336_i2c_client, buf, len);
	}
	else
	{
		ft5336_i2c_client->addr = ft5336_i2c_client->addr & I2C_MASK_FLAG | I2C_DMA_FLAG;
		//MSE_ERR("Sensor dma read timing is %x!\r\n", this_client->timing);
		err = i2c_master_recv(ft5336_i2c_client, CTPI2CDMABuf_pa, len);
		ft5336_i2c_client->addr = ft5336_i2c_client->addr & I2C_MASK_FLAG;
	    if(err < 0)
	    {
			return err;
		}

		for(i = 0; i < len; i++)
		{
			buf[i] = CTPI2CDMABuf_va[i];
		}
		return err;
	}
}

/*
[function]: 
    read out data from ctpm,the destination address is 0.
[parameters]:
    pbt_buf[out]    :point to data buffer;
    bt_len[in]        :the data numbers;    
[return]:
    FTS_TRUE    :success;
    FTS_FALSE    :io fail;
*/
FTS_BOOL byte_read(FTS_BYTE* pbt_buf, FTS_BYTE bt_len)
{
    return i2c_read_interface(I2C_CTPM_ADDRESS, pbt_buf, bt_len);
}

/*
[function]: 
    burn the FW to ctpm.
[parameters]:(ref. SPEC)
    pbt_buf[in]    :point to Head+FW ;
    dw_lenth[in]:the length of the FW + 6(the Head length);    
    bt_ecc[in]    :the ECC of the FW
[return]:
    ERR_OK        :no error;
    ERR_MODE    :fail to switch to UPDATE mode;
    ERR_READID    :read id fail;
    ERR_ERASE    :erase chip fail;
    ERR_STATUS    :status error;
    ERR_ECC        :ecc error.
*/

#define    FTS_PACKET_LENGTH        128

// the macro below is defined for dual tp vendor hw firmware upgrade  compatible
#define FTS_DUAL_VENDOR_COMPAT

#if defined(FTS_DUAL_VENDOR_COMPAT) // phil added 20140529 for truly & tianma compatible
// the macro below is defined for dual tp vendor distinct by lcm name
#define FTS_VENDOR_DISTINCT_BY_LCM

static int compat_fw_ver = 0xff;
#if defined(FTS_VENDOR_DISTINCT_BY_LCM)
#include "lcm_drv.h"
extern const LCM_DRIVER  *lcm_drv;
#endif
#endif

#define IC_FT5X06	0
#define IC_FT5606	1
#define IC_FT5316	2
#define IC_FT5X36	3
#define DEVICE_IC_TYPE	IC_FT5X36

#define    BL_VERSION_LZ4        0
#define    BL_VERSION_Z7        1
#define    BL_VERSION_GZF        2

static E_UPGRADE_ERR_TYPE  fts_ctpm_fw_upgrade(FTS_BYTE* pbt_buf, FTS_DWRD dw_lenth)
{
    FTS_BYTE reg_val[2] = {0};
    FTS_DWRD i = 0;
	u8 	 is_5336_new_bootloader = 0;
	u8 	 is_5336_fwsize_30 = 0;
    FTS_DWRD  packet_number;
    FTS_DWRD  j;
    FTS_DWRD  temp;
    FTS_DWRD  lenght;
    FTS_BYTE  packet_buf[FTS_PACKET_LENGTH + 6];
    FTS_BYTE  auc_i2c_write_buf[10];
    FTS_BYTE bt_ecc;
    int      i_ret;
	int ret=0;
	unsigned char ver;
    /*********Step 1:Reset  CTPM *****/
    /*write 0xaa to register 0xfc*/
	atomic_set(&upgrading, 1);
	mutex_lock(&fwupgrade_mutex);
	mt_eint_mask(CUST_EINT_TOUCH_PANEL_NUM);
#ifdef ESD_CHECK	
 	cancel_delayed_work_sync(&ctp_read_id_work);
#endif
	if(pbt_buf[dw_lenth-12] == 30)
	{
		is_5336_fwsize_30 = 1;
	}
	else 
	{
		is_5336_fwsize_30 = 0;
	}
    //printk("[TSP] is_5336_fwsize_30=%d 0x%x\n",is_5336_fwsize_30,pbt_buf[fw_filenth-12]);
	printk("<suyong> <%d>,%s(),is_5336_fwsize_30=%d\n",__LINE__,__func__,is_5336_fwsize_30 );
	
		/*write 0xaa to register 0xfc*/
	   	ft5x0x_write_reg(0xfc, 0xaa);
		msleep(30);
		
		 /*write 0x55 to register 0xfc*/
		ft5x0x_write_reg(0xfc, 0x55);   
		msleep(30);   

	printk("<suyong> <%d>,%s(),Step 1: Reset CTPM test\n",__LINE__,__func__ );

    /*********Step 2:Enter upgrade mode *****/
    auc_i2c_write_buf[0] = 0x55;
    auc_i2c_write_buf[1] = 0xaa;
    do
    {
        i ++;
        i_ret = ft5x0x_i2c_txdata(auc_i2c_write_buf, 2);
	printk("<suyong> <%d>,%s(),i=%d i_ret=%d\n",__LINE__,__func__,i,i_ret );
		
        delay_qt_ms(5);
    }while(i_ret <= 0 && i < 5 );
	if(i==5)
	{
		ret =-1;
		goto ERR;
	}
	printk("<suyong> <%d>,%s(),Step 2:Enter upgrade mode\n",__LINE__,__func__ );
	
    /*********Step 3:check READ-ID***********************/        
    cmd_write(0x90,0x00,0x00,0x00,4);
	i=0;
	i_ret=0;
    do
    {
        i ++;
        i_ret = byte_read(reg_val,2);
        delay_qt_ms(10);
    }while(i_ret <= 0 && i < 5 );
	if(i==5)
	{
		ret =-1;
		goto ERR;
	}

	printk("<suyong> <%d>,%s(),%x  %x\n",__LINE__,__func__,reg_val[0],reg_val[1] );
        //printk("[TSP] Step 2: CTPM ID,ID1 = 0x%x,ID2 = 0x%x\n",reg_val[0],reg_val[1]);
	auc_i2c_write_buf[0] = 0xcd;
	cmd_write(auc_i2c_write_buf[0],0x00,0x00,0x00,1);
	byte_read(reg_val,1);

//   ft5206_i2c_Read(i2c_client, auc_i2c_write_buf, 1, reg_val, 1);
	/*********0705 mshl ********************/
	/*if (reg_val[0] > 4)
		is_5336_new_bootloader = 1;*/
	if (reg_val[0] <= 4)
	{
		is_5336_new_bootloader = BL_VERSION_LZ4 ;
	}
	else if(reg_val[0] == 7)
	{
		is_5336_new_bootloader = BL_VERSION_Z7 ;
	}
	else if(reg_val[0] >= 0x0f)
	{
		is_5336_new_bootloader = BL_VERSION_GZF ;
	}
	printk("<suyong> <%d>,%s(),reg_val[0]=%d is_5336_new_bootloader=%d\n",__LINE__,__func__,reg_val[0],is_5336_new_bootloader );
	printk("<suyong> <%d>,%s(),Step 3:check READ-ID\n",__LINE__,__func__ );

//	printk("<TSP> <%d>,%s(),is_5336_new_bootloader=%d  0x%x\n",__LINE__,__func__ ,is_5336_new_bootloader,reg_val[0]);
     /*********Step 4:erase app*******************************/
	 /*********Step 4:erase app and panel paramenter area ********************/
	if(is_5336_fwsize_30)
//	if (0)
	{
		auc_i2c_write_buf[0] = 0x61;
		cmd_write(auc_i2c_write_buf[0],0x00,0x00,0x00,1);
		delay_qt_ms(4000);
		auc_i2c_write_buf[0] = 0x63;
		cmd_write(auc_i2c_write_buf[0],0x00,0x00,0x00,1);
		msleep(50);
	}
	else
	{
		auc_i2c_write_buf[0] = 0x61;
		cmd_write(auc_i2c_write_buf[0],0x00,0x00,0x00,1);
		delay_qt_ms(4000);

	}
	printk("<suyong> <%d>,%s(),Step 4: erase\n",__LINE__,__func__ );
  
    
    //printk("[TSP] Step 4: erase.ret=%d\n",ret);

    /*********Step 5:write firmware(FW) to ctpm flash*********/
    bt_ecc = 0;
   // printk("[TSP] Step 5: start upgrade. \n");
	if(is_5336_new_bootloader == BL_VERSION_LZ4 || is_5336_new_bootloader == BL_VERSION_Z7 )
	{
		dw_lenth = dw_lenth - 8;
	}
	else if(is_5336_new_bootloader == BL_VERSION_GZF) 
	{
		dw_lenth = dw_lenth - 14;
	}
    packet_number = (dw_lenth) / FTS_PACKET_LENGTH;
    packet_buf[0] = 0xbf;
    packet_buf[1] = 0x00;
    for (j=0;j<packet_number;j++)
    {
        temp = j * FTS_PACKET_LENGTH;
        packet_buf[2] = (FTS_BYTE)(temp>>8);
        packet_buf[3] = (FTS_BYTE)temp;
        lenght = FTS_PACKET_LENGTH;
        packet_buf[4] = (FTS_BYTE)(lenght>>8);
        packet_buf[5] = (FTS_BYTE)lenght;

        for (i=0;i<FTS_PACKET_LENGTH;i++)
        {
            packet_buf[6+i] = pbt_buf[j*FTS_PACKET_LENGTH + i]; 
            bt_ecc ^= packet_buf[6+i];
        }
        
        ret=CTPDMA_i2c_write(0x70, &packet_buf[0],FTS_PACKET_LENGTH + 6);
		if(ret <0)
		{
			printk("<suyong> <%d>,%s(),ret=%d\n",__LINE__,__func__,ret );
			goto ERR;
		}
              //printk("[TSP] 111 ret 0x%x \n", ret);
        //delay_qt_ms(FTS_PACKET_LENGTH/6 + 1);
        if ((j * FTS_PACKET_LENGTH % 1024) == 0)
        {
              //printk("[TSP] upgrade the 0x%x th byte.\n", ((unsigned int)j) * FTS_PACKET_LENGTH);
        }
		msleep(FTS_PACKET_LENGTH/6 + 1);
    }

    if ((dw_lenth) % FTS_PACKET_LENGTH > 0)
    {
        temp = packet_number * FTS_PACKET_LENGTH;
        packet_buf[2] = (FTS_BYTE)(temp>>8);
        packet_buf[3] = (FTS_BYTE)temp;

        temp = (dw_lenth) % FTS_PACKET_LENGTH;
        packet_buf[4] = (FTS_BYTE)(temp>>8);
        packet_buf[5] = (FTS_BYTE)temp;

        for (i=0;i<temp;i++)
        {
            packet_buf[6+i] = pbt_buf[ packet_number*FTS_PACKET_LENGTH + i]; 
            bt_ecc ^= packet_buf[6+i];
        }
             // printk("[TSP]temp 0x%x \n", temp);
        ret = CTPDMA_i2c_write(0x70, &packet_buf[0],temp+6);
		if(ret <0)
		{
			printk("<suyong> <%d>,%s(),ret=%d\n",__LINE__,__func__,ret );
			goto ERR;
		}
              //printk("[TSP] 222 ret 0x%x \n", ret);
        delay_qt_ms(20);
    }

    //send the last six byte
	if(is_5336_new_bootloader == BL_VERSION_LZ4 || is_5336_new_bootloader == BL_VERSION_Z7 )
	{
	    for (i = 0; i<6; i++)
	    {
			if (is_5336_new_bootloader	== BL_VERSION_Z7 && DEVICE_IC_TYPE==IC_FT5X36) 
			{
				temp = 0x7bfa + i;
			}
			else if(is_5336_new_bootloader == BL_VERSION_LZ4)
			{
				temp = 0x6ffa + i;
			}
	        packet_buf[2] = (FTS_BYTE)(temp>>8);
	        packet_buf[3] = (FTS_BYTE)temp;
	        temp =1;
	        packet_buf[4] = (FTS_BYTE)(temp>>8);
	        packet_buf[5] = (FTS_BYTE)temp;
	        packet_buf[6] = pbt_buf[ dw_lenth + i]; 
	        bt_ecc ^= packet_buf[6];
	        ret =CTPDMA_i2c_write(0x70,&packet_buf[0],7);  
			if(ret <0)
			{
				printk("<suyong> <%d>,%s(),ret=%d\n",__LINE__,__func__,ret );
				goto ERR;
			}

	        delay_qt_ms(20);
	    }
	}
	else if(is_5336_new_bootloader == BL_VERSION_GZF)
	{
		for (i = 0; i<12; i++)
		{
			if (is_5336_fwsize_30 && DEVICE_IC_TYPE==IC_FT5X36) 
			{
				temp = 0x7ff4 + i;
			}
			else if (DEVICE_IC_TYPE==IC_FT5X36) 
			{
				temp = 0x7bf4 + i;
			}
			packet_buf[2] = (u8)(temp>>8);
			packet_buf[3] = (u8)temp;
			temp =1;
			packet_buf[4] = (u8)(temp>>8);
			packet_buf[5] = (u8)temp;
			packet_buf[6] = pbt_buf[ dw_lenth + i]; 
			bt_ecc ^= packet_buf[6];
  
			ret=CTPDMA_i2c_write(0x70, &packet_buf[0],7);
			if(ret <0)
			{
				printk("<suyong> <%d>,%s(),ret=%d\n",__LINE__,__func__,ret );
				goto ERR;
			}
			msleep(10);

		}
	}
	printk("<suyong> <%d>,%s(),write firmware(FW)\n",__LINE__,__func__ );
	
    /*********Step 6: read out checksum***********************/
    /*send the opration head*/
    //cmd_write(0xcc,0x00,0x00,0x00,1);
    //byte_read(reg_val,1);
i2c_smbus_read_i2c_block_data(ft5336_i2c_client, 0xcc, 1, &(reg_val[0]));
	printk("<suyong> <%d>,%s(),ecc read 0x%x, new firmware 0x%x\n",__LINE__,__func__ ,reg_val[0], bt_ecc);
    //printk("[TSP] Step 6:  ecc read 0x%x, new firmware 0x%x. \n", reg_val[0], bt_ecc);
    if(reg_val[0] != bt_ecc)
    {
        //return ERR_ECC;
        ret=-1;
        goto ERR;
    }
	printk("<suyong> <%d>,%s(),Step 6: read out checksum\n",__LINE__,__func__ );

    /*********Step 7: reset the new FW***********************/
    //cmd_write(0x07,0x00,0x00,0x00,1);
	mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ZERO);  
    msleep(1);  
    mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ONE);
	ret=0;
	printk("<suyong> <%d>,%s(),reset the new FW\n",__LINE__,__func__ );
ERR:
	mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM); 
	mutex_unlock(&fwupgrade_mutex);
	atomic_set(&upgrading, 0);
    return ret;
}

#if (0)  //使用新的升级函数,主要是新函数判断了升级的类型和地址等 苏 勇 2014年01月15日 17:47:38
E_UPGRADE_ERR_TYPE  fts_ctpm_fw_upgrade(FTS_BYTE* pbt_buf, FTS_DWRD dw_lenth)
{
    FTS_BYTE reg_val[2] = {0};
    FTS_DWRD i = 0;

    FTS_DWRD  packet_number;
    FTS_DWRD  j;
    FTS_DWRD  temp;
    FTS_DWRD  lenght;
    FTS_BYTE  packet_buf[FTS_PACKET_LENGTH + 6];
    FTS_BYTE  auc_i2c_write_buf[10];
    FTS_BYTE bt_ecc;
    int      i_ret;
	int ret=0;
	unsigned char ver;
    /*********Step 1:Reset  CTPM *****/
    /*write 0xaa to register 0xfc*/
	atomic_set(&upgrading, 1);
	mutex_lock(&fwupgrade_mutex);
	mt_eint_mask(CUST_EINT_TOUCH_PANEL_NUM);
#ifdef ESD_CHECK	
 	cancel_delayed_work_sync(&ctp_read_id_work);
#endif
	

	mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ZERO);  
    msleep(10);  
    mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ONE);
    //printk("[TSP] Step 1: Reset CTPM test\n");
   
    delay_qt_ms(50);   
    /*********Step 2:Enter upgrade mode *****/
    auc_i2c_write_buf[0] = 0x55;
    auc_i2c_write_buf[1] = 0xaa;
    do
    {
        i ++;
        i_ret = ft5x0x_i2c_txdata(auc_i2c_write_buf, 2);
        delay_qt_ms(5);
    }while(i_ret <= 0 && i < 5 );
	if(i==5)
	{
		ret =-1;
		goto ERR;
	}
    /*********Step 3:check READ-ID***********************/        
    cmd_write(0x90,0x00,0x00,0x00,4);
	i=0;
	i_ret=0;
    do
    {
        i ++;
        i_ret = byte_read(reg_val,2);
        delay_qt_ms(10);
    }while(i_ret <= 0 && i < 5 );
	if(i==5)
	{
		ret =-1;
		goto ERR;
	}
// 苏 勇 2013年11月19日 20:15:34	printk("<suyong> <%d>,%s(),CTPM ID,ID1 = 0x%x,ID2 = 0x%x\n",__LINE__,__func__,reg_val[0],reg_val[1] );
        //printk("[TSP] Step 2: CTPM ID,ID1 = 0x%x,ID2 = 0x%x\n",reg_val[0],reg_val[1]);
#if 0 /*zhouwl, temp disable this line???*/
    if (reg_val[0] == 0x79 && reg_val[1] == 0x3)
    {
        printk("[TSP] Step 3: CTPM ID,ID1 = 0x%x,ID2 = 0x%x\n",reg_val[0],reg_val[1]);
    }
    else
    {
        return ERR_READID;
        //i_is_new_protocol = 1;
    }
#endif
     /*********Step 4:erase app*******************************/
    ret = cmd_write(0x61,0x00,0x00,0x00,1);

	if(ret <0)
	{
// 苏 勇 2013年11月19日 20:15:38		printk("<suyong> <%d>,%s(),ret=%d\n",__LINE__,__func__,ret );
		goto ERR;
	}
   
    delay_qt_ms(4000);
    //printk("[TSP] Step 4: erase.ret=%d\n",ret);

    /*********Step 5:write firmware(FW) to ctpm flash*********/
    bt_ecc = 0;
   // printk("[TSP] Step 5: start upgrade. \n");
    dw_lenth = dw_lenth - 8;
    packet_number = (dw_lenth) / FTS_PACKET_LENGTH;
    packet_buf[0] = 0xbf;
    packet_buf[1] = 0x00;
    for (j=0;j<packet_number;j++)
    {
        temp = j * FTS_PACKET_LENGTH;
        packet_buf[2] = (FTS_BYTE)(temp>>8);
        packet_buf[3] = (FTS_BYTE)temp;
        lenght = FTS_PACKET_LENGTH;
        packet_buf[4] = (FTS_BYTE)(lenght>>8);
        packet_buf[5] = (FTS_BYTE)lenght;

        for (i=0;i<FTS_PACKET_LENGTH;i++)
        {
            packet_buf[6+i] = pbt_buf[j*FTS_PACKET_LENGTH + i]; 
            bt_ecc ^= packet_buf[6+i];
        }
        
        ret=CTPDMA_i2c_write(0x70, &packet_buf[0],FTS_PACKET_LENGTH + 6);
		if(ret <0)
		{
			goto ERR;
		}
              //printk("[TSP] 111 ret 0x%x \n", ret);
        //delay_qt_ms(FTS_PACKET_LENGTH/6 + 1);
        if ((j * FTS_PACKET_LENGTH % 1024) == 0)
        {
              //printk("[TSP] upgrade the 0x%x th byte.\n", ((unsigned int)j) * FTS_PACKET_LENGTH);
        }
    }

    if ((dw_lenth) % FTS_PACKET_LENGTH > 0)
    {
        temp = packet_number * FTS_PACKET_LENGTH;
        packet_buf[2] = (FTS_BYTE)(temp>>8);
        packet_buf[3] = (FTS_BYTE)temp;

        temp = (dw_lenth) % FTS_PACKET_LENGTH;
        packet_buf[4] = (FTS_BYTE)(temp>>8);
        packet_buf[5] = (FTS_BYTE)temp;

        for (i=0;i<temp;i++)
        {
            packet_buf[6+i] = pbt_buf[ packet_number*FTS_PACKET_LENGTH + i]; 
            bt_ecc ^= packet_buf[6+i];
        }
             // printk("[TSP]temp 0x%x \n", temp);
        ret = CTPDMA_i2c_write(0x70, &packet_buf[0],temp+6);
		if(ret <0)
		{
			goto ERR;
		}
        delay_qt_ms(20);
    }

    //send the last six byte
    for (i = 0; i<6; i++)
    {
        temp = 0x6ffa + i;
        packet_buf[2] = (FTS_BYTE)(temp>>8);
        packet_buf[3] = (FTS_BYTE)temp;
        temp =1;
        packet_buf[4] = (FTS_BYTE)(temp>>8);
        packet_buf[5] = (FTS_BYTE)temp;
        packet_buf[6] = pbt_buf[ dw_lenth + i]; 
        bt_ecc ^= packet_buf[6];
        ret =CTPDMA_i2c_write(0x70,&packet_buf[0],7);  
		if(ret <0)
		{
			goto ERR;
		}

        delay_qt_ms(20);
    }

    /*********Step 6: read out checksum***********************/
    /*send the opration head*/
    //cmd_write(0xcc,0x00,0x00,0x00,1);
    //byte_read(reg_val,1);
i2c_smbus_read_i2c_block_data(ft5336_i2c_client, 0xcc, 1, &(reg_val[0]));
    if(reg_val[0] != bt_ecc)
    {
        //return ERR_ECC;
        ret=-1;
        goto ERR;
    }

    /*********Step 7: reset the new FW***********************/
    //cmd_write(0x07,0x00,0x00,0x00,1);
	mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ZERO);  
    msleep(1);  
    mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ONE);
	ret=0;
ERR:
	mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM); 
	mutex_unlock(&fwupgrade_mutex);
	atomic_set(&upgrading, 0);
    return ret;
}
#endif /* 0 */

static int fts_ctpm_fw_upgrade_with_i_file(void)
{
   FTS_BYTE*     pbt_buf = FTS_NULL;
   int i_ret;
   unsigned char version=0;
    FTS_BYTE flag;
    FTS_DWRD i = 0;
    //=========FW upgrade========================*/
#if defined(FTS_DUAL_VENDOR_COMPAT) //phil add 20140529 for tianma & truly compatible
	int chipID = ft5x0x_read_ID_ver();
	int fw_len = 0;
	printk("[TSP]ID_ver=%x, fw_ver=%x\n", chipID, ft5x0x_read_fw_ver());
	#if defined(FTS_VENDOR_DISTINCT_BY_LCM)
	if(strcmp(lcm_drv->name,LCM_NAME1)==0
	#if defined(LCM_NAME11)
    || strcmp(lcm_drv->name,LCM_NAME11)==0
    #endif
    ) // truly TP's ID is 0x5a}
	#else
	if(chipID == CTPM_FW[sizeof(CTPM_FW)-1])
	#endif
	{
		fw_len = sizeof(CTPM_FW);
		pbt_buf = CTPM_FW;
		compat_fw_ver = pbt_buf[fw_len-2];
	}
	#if defined(FTS_VENDOR_DISTINCT_BY_LCM)
	else if(strcmp(lcm_drv->name,LCM_NAME2)==0) // tianma TP's ID is 0x55
	#else
	else if(chipID == CTPM_FW2[sizeof(CTPM_FW2)-1])
	#endif
	{
		fw_len = sizeof(CTPM_FW2);
		pbt_buf = CTPM_FW2;
		compat_fw_ver = pbt_buf[fw_len-2];
	}
	else
	{
		return 0;
	}
	i_ret =  fts_ctpm_fw_upgrade(pbt_buf,fw_len);
#else
	pbt_buf = CTPM_FW;
	
	printk("version=%x ,pbt_buf[sizeof(CTPM_FW)-2]=%d\n",version,pbt_buf[sizeof(CTPM_FW)-2]);
	printk("[TSP]ID_ver=%x, fw_ver=%x\n", ft5x0x_read_ID_ver(), ft5x0x_read_fw_ver());
#if 0 /*zhouwl, temp disable this line*/
if(0xa8 != ft5x0x_read_ID_ver())
{
	if(ft5x0x_read_ID_ver() != pbt_buf[sizeof(CTPM_FW)-1])
	{
        return;
	}
	
    do
    {
        i ++;
        version =ft5x0x_read_fw_ver();
        delay_qt_ms(2);
    }while( i < 5 );
    
	if(version==pbt_buf[sizeof(CTPM_FW)-2])
	{
		return;
	}
}
#endif
   /*call the upgrade function*/
   i_ret =  fts_ctpm_fw_upgrade(pbt_buf,sizeof(CTPM_FW));
#endif
   if (i_ret != 0)
   {
	printk("[TSP]upgrade error\n");
       //error handling ...
       //TBD
   }
	msleep(200);  
    ft5x0x_write_reg(0xfc,0x04);
	msleep(4000);
	flag=0;
	i2c_smbus_read_i2c_block_data(ft5336_i2c_client, 0xFC, 1, &flag);
	//printk("flag=%d\n",flag);
   return i_ret;
}

unsigned char fts_ctpm_get_upg_ver(void)
{
    unsigned int ui_sz;
#if defined(FTS_DUAL_VENDOR_COMPAT) // phil added 20140529 for try
	return compat_fw_ver;
#else
    ui_sz = sizeof(CTPM_FW);
    if (ui_sz > 2)
    {
        return CTPM_FW[ui_sz - 2];
    }
    else
    {
        //TBD, error handling?
        return 0xff; //default value
    }
#endif
}

static void tpd_resume( struct early_suspend *h );
static unsigned char CtpFwUpgradeForIOCTRL(unsigned char* pbt_buf, unsigned int dw_lenth)
{
	int ret=0;
	
	tpd_resume((struct early_suspend *)0);
	ret=fts_ctpm_fw_upgrade(pbt_buf,dw_lenth);

	msleep(200);  
    ft5x0x_write_reg(0xfc,0x04);
	msleep(4000);
   	return ret;
}
#endif
#ifdef ESD_CHECK	
static void ESD_read_id_workqueue(struct work_struct *work)
{
	char data;
	if(tpd_halt) 
		return; 
	i2c_smbus_read_i2c_block_data(ft5336_i2c_client, 0x88, 1, &data);
//	TPD_DEBUG("ESD_read_id_workqueue data: %d\n", data);
	printk("ESD_read_id_workqueue data: %d\n", data);
	if((data > 5)&&(data < 10))
	{
		//add_timer();
	}
	else
	{

	 	mt_eint_mask(CUST_EINT_TOUCH_PANEL_NUM);
		 if(tpd_state)
		 {
			 input_mt_sync(tpd->dev);
	                input_sync(tpd->dev);
			tpd_state = 0;
		 }
		msleep(5);  
	
#ifdef TPD_POWER_SOURCE_CUSTOM
    		hwPowerDown(TPD_POWER_SOURCE_CUSTOM,  "TP");
#else
    		hwPowerDown(MT65XX_POWER_LDO_VGP2,  "TP");
#endif
#ifdef TPD_POWER_SOURCE_1800
    		hwPowerDown(TPD_POWER_SOURCE_1800,  "TP");
#endif    
		msleep(5);  
#ifdef TPD_POWER_SOURCE_CUSTOM
    		hwPowerOn(TPD_POWER_SOURCE_CUSTOM, VOL_2800, "TP");
#else
    		hwPowerOn(MT65XX_POWER_LDO_VGP2, VOL_2800, "TP");
#endif
#ifdef TPD_POWER_SOURCE_1800
    		hwPowerOn(TPD_POWER_SOURCE_1800, VOL_1800, "TP");
#endif    
//#endif	
		msleep(100);
		mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
		mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
		mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ZERO);  
		msleep(10);  
		mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
		mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
		mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ONE);
	 	 mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM); 
		 
		 msleep(200);
	}
	if(tpd_halt) 
	{
	}
	else 
		queue_delayed_work(ctp_read_id_workqueue, &ctp_read_id_work,400); //schedule a work for the first detection					

}
#endif
#ifdef FTS_PRESSURE
static  void tpd_down(int x, int y, int p, int w,int a) {
#else
static  void tpd_down(int x, int y, int p) {
#endif
	// input_report_abs(tpd->dev, ABS_PRESSURE, p);
     if (RECOVERY_BOOT == get_boot_mode())
     {
     }
	 else
	 {
	 	input_report_key(tpd->dev, BTN_TOUCH, 1);
	 }
	 #ifdef FTS_PRESSURE
	 input_report_abs(tpd->dev, ABS_MT_PRESSURE, w);
	 input_report_abs(tpd->dev, ABS_MT_TOUCH_MAJOR, a);
	 #else
	 input_report_abs(tpd->dev, ABS_MT_PRESSURE, 1);
	 input_report_abs(tpd->dev, ABS_MT_TOUCH_MAJOR, 20);
	 #endif
	 input_report_abs(tpd->dev, ABS_MT_POSITION_X, x);
	 input_report_abs(tpd->dev, ABS_MT_POSITION_Y, y);
	 /* Lenovo-sw yexm1, optimize the code, 2012-9-19 begin */
	// printk("D[%4d %4d %4d] ", x, y, p);
	/* Lenovo-sw yexm1, optimize the code, 2012-9-19 end */
	 /* track id Start 0 */
       input_report_abs(tpd->dev, ABS_MT_TRACKING_ID, p); 
	 input_mt_sync(tpd->dev);
     if (FACTORY_BOOT == get_boot_mode()|| RECOVERY_BOOT == get_boot_mode())
     {   
       tpd_button(x, y, 1);  
     }
	 if(y > TPD_RES_Y) //virtual key debounce to avoid android ANR issue
	 {
         /* Lenovo-sw yexm1 modify, 2012-10-15, delete the delay */
         //msleep(50);
		 printk("D virtual key \n");
	 }
	 TPD_EM_PRINT(x, y, x, y, p-1, 1);
 }
 
static  void tpd_up(int x, int y,int *count) {
	 //if(*count>0) {
	 	#ifdef FTS_PRESSURE
		input_report_abs(tpd->dev, ABS_MT_PRESSURE, 0);
		input_report_abs(tpd->dev, ABS_MT_TOUCH_MAJOR, 0);
		#else
		input_report_abs(tpd->dev, ABS_MT_PRESSURE, 0);
		#endif
		 input_report_key(tpd->dev, BTN_TOUCH, 0);
		 //input_report_abs(tpd->dev, ABS_MT_TOUCH_MAJOR, 0);
		 //input_report_abs(tpd->dev, ABS_MT_POSITION_X, x);
		 //input_report_abs(tpd->dev, ABS_MT_POSITION_Y, y);
		 //printk("U[%4d %4d %4d] ", x, y, 0);
		 input_mt_sync(tpd->dev);
		 TPD_EM_PRINT(x, y, x, y, 0, 0);
	//	 (*count)--;
     if (FACTORY_BOOT == get_boot_mode()|| RECOVERY_BOOT == get_boot_mode())
     {   
        tpd_button(x, y, 0); 
     }   		 

 }

 static int tpd_touchinfo(struct touch_info *cinfo, struct touch_info *pinfo)
 {

	int i = 0;
	char data[(3+6*(TPD_MAX_PONIT-1)+3+1+7)/8*8] = {0};

    u16 high_byte,low_byte;
	u8 report_rate =0;
	u8 version=0xff;

	//p_point_num = point_num;
	i2c_smbus_read_i2c_block_data(ft5336_i2c_client, 0x00, 3, &(data[0]));

		/* Device Mode[2:0] == 0 :Normal operating Mode*/
	if(data[0] & 0x70 != 0) return false; 

	/*get the number of the touch points*/
	point_num= data[2] & 0x0f;

	if(point_num>TPD_MAX_PONIT)
	{
		// Delete touchscreen logspam, Pablito2020
	    //printk("error ft5336 point_num(%d)>TPD_MAX_PONIT(%d)\n",point_num,TPD_MAX_PONIT);
		point_num=TPD_MAX_PONIT;
	}

//	for (i<0;i<(3+6*(TPD_MAX_PONIT-1)+3+1+7)/8;i++)
	for (i=0;i<(6*point_num+7)/8;i++)

	{
		i2c_smbus_read_i2c_block_data(ft5336_i2c_client, 0x03+i*8, 8, &(data[3+i*8]));
	}		
		for(i = 0; i < point_num; i++)
		{
			cinfo->p[i] = data[3+6*i] >> 6; //event flag 
                     cinfo->id[i] = data[3+6*i+2] >> 4; //touch id

	       /*get the X coordinate, 2 bytes*/
			high_byte = data[3+6*i];
			high_byte <<= 8;
			high_byte &= 0x0f00;
			low_byte = data[3+6*i + 1];
			cinfo->x[i] = high_byte |low_byte;

				//cinfo->x[i] =  cinfo->x[i] * 480 >> 11; //calibra
		
			/*get the Y coordinate, 2 bytes*/
			
			high_byte = data[3+6*i+2];
			high_byte <<= 8;
			high_byte &= 0x0f00;
			low_byte = data[3+6*i+3];
			cinfo->y[i] = high_byte |low_byte;

			  //cinfo->y[i]=  cinfo->y[i] * 800 >> 11;
		
			cinfo->count++;

			#ifdef FTS_PRESSURE
			cinfo->au8_touch_weight[i]=(data[3+6 * i+4]*5/2); //data[3+6 * i+4]
			cinfo->au8_touch_area[i]=((data[3+6 * i+5]&0xf0) >> 2);//data[3+6 * i+5] >> 4
			#endif
			//printk("ft5336 Point[%d]:[x=%d,y=%d,p=%x,id=%x,w=%d,a=%d]\n",i,cinfo->x[i],cinfo->y[i],cinfo->p[i],cinfo->id[i],cinfo->au8_touch_weight[i],cinfo->au8_touch_area[i]);
		}
		//printk("ft5336 cinfo->x[0] = %d, cinfo->y[0] = %d, cinfo->p[0] = %d\n", cinfo->x[0], cinfo->y[0], cinfo->p[0]);	
		//printk("ft5336 cinfo->x[1] = %d, cinfo->y[1] = %d, cinfo->p[1] = %d\n", cinfo->x[1], cinfo->y[1], cinfo->p[1]);		
		//printk("ft5336 cinfo->x[2]= %d, cinfo->y[2]= %d, cinfo->p[2] = %d\n", cinfo->x[2], cinfo->y[2], cinfo->p[2]);	
		  
	 return true;

 };

 static int touch_event_handler(void *unused)
 {
  
    struct touch_info cinfo, pinfo;
	 int i=0;

	 struct sched_param param = { .sched_priority = 91 };
	 sched_setscheduler(current, SCHED_RR, &param);
 
	 do
	 {
		 set_current_state(TASK_INTERRUPTIBLE); 
		  wait_event_interruptible(waiter,tpd_flag!=0);
						 
			 tpd_flag = 0;
			 
		 set_current_state(TASK_RUNNING);

		  if (tpd_touchinfo(&cinfo, &pinfo)) 
		  {
		    //TPD_DEBUG("point_num = %d\n",point_num);
			TPD_DEBUG_SET_TIME;


            if(point_num >0) 
		{
			int i=0;
		     	for (i=0;i<point_num;i++)
		     	{
					//tpd_down(cinfo.x[i], cinfo.y[i], i+1);
					#ifdef FTS_PRESSURE
					tpd_down(cinfo.x[i], cinfo.y[i], cinfo.id[i], cinfo.au8_touch_weight[i], cinfo.au8_touch_area[i]);
					#else
					tpd_down(cinfo.x[i], cinfo.y[i], cinfo.id[i]);
					#endif
		     	}
                input_sync(tpd->dev);				
            } 
			else  
            {
			    tpd_up(cinfo.x[0], cinfo.y[0], 0);
                //TPD_DEBUG("release --->\n"); 
                //input_mt_sync(tpd->dev);
                input_sync(tpd->dev);
            }
        }
        	  mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM); 

 }while(!kthread_should_stop());
 			  mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM); 

	 return 0;
 }
 
 static int tpd_detect (struct i2c_client *client, struct i2c_board_info *info) 
 {
	 strcpy(info->type, TPD_DEVICE);	
	  return 0;
 }
 
 static void tpd_eint_interrupt_handler(void)
 {
	 //TPD_DEBUG("TPD interrupt has been triggered\n");
 		  mt_eint_mask(CUST_EINT_TOUCH_PANEL_NUM); 

	 TPD_DEBUG_PRINT_INT;
	 tpd_flag = 1;
	 wake_up_interruptible(&waiter);
	 
 }
 static int tpd_probe(struct i2c_client *client, const struct i2c_device_id *id)
 {	 
	int retval = TPD_OK;
#ifdef ESD_CHECK	
	int ret;
#endif
	char data;
	u8 report_rate=0;
	int err=0;
	int reset_count = 0;

reset_proc:   
	ft5336_i2c_client = client;
			//power on, need confirm with SA 
#ifdef TPD_POWER_SOURCE_CUSTOM
	hwPowerOn(TPD_POWER_SOURCE_CUSTOM, VOL_2800, "TP");
#else
    	hwPowerOn(MT65XX_POWER_LDO_VGP2, VOL_2800, "TP");
#endif
#ifdef TPD_POWER_SOURCE_1800
	hwPowerOn(TPD_POWER_SOURCE_1800, VOL_1800, "TP");
#endif 

	#ifdef TPD_CLOSE_POWER_IN_SLEEP	 
	hwPowerDown(TPD_POWER_SOURCE,"TP");
	hwPowerOn(TPD_POWER_SOURCE,VOL_3300,"TP");
	msleep(100);
	#else
	mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ZERO);  
	msleep(100);
	TPD_DMESG(" ft5336 reset\n");
	mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ONE);
    msleep(300);
	#endif
	TPD_DMESG(" ft5336 init eint\n");
	mt_set_gpio_mode(GPIO_CTP_EINT_PIN, GPIO_CTP_EINT_PIN_M_EINT);
    mt_set_gpio_dir(GPIO_CTP_EINT_PIN, GPIO_DIR_IN);
    mt_set_gpio_pull_enable(GPIO_CTP_EINT_PIN, GPIO_PULL_ENABLE);
    mt_set_gpio_pull_select(GPIO_CTP_EINT_PIN, GPIO_PULL_UP);
 
	  //mt65xx_eint_set_sens(CUST_EINT_TOUCH_PANEL_NUM, CUST_EINT_TOUCH_PANEL_SENSITIVE);
	  //mt65xx_eint_set_hw_debounce(CUST_EINT_TOUCH_PANEL_NUM, CUST_EINT_TOUCH_PANEL_DEBOUNCE_CN);
	  mt_eint_registration(CUST_EINT_TOUCH_PANEL_NUM, CUST_EINT_TOUCH_PANEL_TYPE, tpd_eint_interrupt_handler, 1);
	  mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM);
 
	msleep(100);
#ifdef CONFIG_SUPPORT_FTS_CTP_UPG
	CTPI2CDMABuf_va = (u8 *)dma_alloc_coherent(NULL, 4096, &CTPI2CDMABuf_pa, GFP_KERNEL);
    	if(!CTPI2CDMABuf_va)
			{
    		printk("[TSP] dma_alloc_coherent error\n");
	}
#endif 
	if((i2c_smbus_read_i2c_block_data(ft5336_i2c_client, 0x00, 1, &data))< 0)
	{
		TPD_DMESG("I2C transfer error, line: %d\n", __LINE__);
#ifdef TPD_RESET_ISSUE_WORKAROUND
        if ( reset_count < TPD_MAX_RESET_COUNT )
        {
            reset_count++;
            goto reset_proc;
        }
#endif
		   return -1; 
	   }

	//set report rate 80Hz
	report_rate = 0x8; 
	if((i2c_smbus_write_i2c_block_data(ft5336_i2c_client, 0x88, 1, &report_rate))< 0)
	{
	    if((i2c_smbus_write_i2c_block_data(ft5336_i2c_client, 0x88, 1, &report_rate))< 0)
	    {
		   TPD_DMESG("I2C read report rate error, line: %d\n", __LINE__);
	    }
		   
	}

	tpd_load_status = 1;

	#ifdef VELOCITY_CUSTOM_FT5206
	if((err = misc_register(&tpd_misc_device)))

	{
		// Delete touchscreen logspam, Pablito2020
		//printk("mtk_tpd: tpd_misc_device register failed\n");
	}
	#endif
 #if 0//def CONFIG_SUPPORT_FTS_CTP_UPG
    	printk("[TSP] Step 0:init \n");
	msleep(100);
	fts_ctpm_fw_upgrade_with_i_file();
    	printk("[TSP] Step 8:init stop\n");
	printk("[wj]the version is 0x%02x.\n", ft5x0x_read_fw_ver());
	//if((i2c_smbus_read_i2c_block_data(i2c_client, TOUCH_FMV_ID, 1, &data))< 0)
	//	{
	//	TPD_DMESG("I2C transfer error, line: %d\n", __LINE__);
		//return -1;
	//	}
#endif	

#ifdef ESD_CHECK	
	ctp_read_id_workqueue = create_workqueue("ctp_read_id");
	INIT_DELAYED_WORK(&ctp_read_id_work, ESD_read_id_workqueue);
	ret = queue_delayed_work(ctp_read_id_workqueue, &ctp_read_id_work,400); //schedule a work for the first detection					
    	printk("[TSP] ret =%d\n",ret);
	#endif
//end
	#ifdef FTS_CTL_IIC
	if (ft_rw_iic_drv_init(client) < 0)
		dev_err(&client->dev, "%s:[FTS] create fts control iic driver failed\n",
				__func__);
    #endif
	ft5336_thread = kthread_run(touch_event_handler, 0, TPD_DEVICE);
	 if (IS_ERR(ft5336_thread))
		 { 
		  retval = PTR_ERR(ft5336_thread);
		  TPD_DMESG(TPD_DEVICE " failed to create kernel thread: %d\n", retval);
		}

	TPD_DMESG("ft5336 Touch Panel Device Probe %s\n", (retval < TPD_OK) ? "FAIL" : "PASS");

#if defined (CONFIG_SUPPORT_FTS_CTP_UPG)
	atomic_set(&upgrading, 0);
#endif /* CONFIG_SUPPORT_FTS_CTP_UPG */
   return 0;
   
 }

 static int tpd_remove(struct i2c_client *client)
 
 {
        int err;
	 TPD_DEBUG("TPD removed\n");
#ifdef CONFIG_SUPPORT_FTS_CTP_UPG
	 
	if(CTPI2CDMABuf_va)
	{
		dma_free_coherent(NULL, 4096, CTPI2CDMABuf_va, CTPI2CDMABuf_pa);
		CTPI2CDMABuf_va = NULL;
		CTPI2CDMABuf_pa = 0;
	}
#endif	
#ifdef ESD_CHECK	
	destroy_workqueue(ctp_read_id_workqueue);
#endif	
#ifdef FTS_CTL_IIC
   ft_rw_iic_drv_exit();
#endif	
   return 0;
 }
 
 
 static int tpd_local_init(void)
 {

 
  TPD_DMESG("Focaltech FT5366 I2C Touchscreen Driver (Built %s @ %s)\n", __DATE__, __TIME__);
 
 
   if(i2c_add_driver(&tpd_i2c_driver)!=0)
   	{
  		TPD_DMESG("ft5366 unable to add i2c driver.\n");
      	return -1;
    }
    if(tpd_load_status == 0) 
    {
    	TPD_DMESG("ft5366 add error touch panel driver.\n");
    	i2c_del_driver(&tpd_i2c_driver);
    	return -1;
    }
	
#ifdef TPD_HAVE_BUTTON     
    tpd_button_setting(TPD_KEY_COUNT, tpd_keys_local, tpd_keys_dim_local);// initialize tpd button data
#endif   
  
#if (defined(TPD_WARP_START) && defined(TPD_WARP_END))    
    TPD_DO_WARP = 1;
    memcpy(tpd_wb_start, tpd_wb_start_local, TPD_WARP_CNT*4);
    memcpy(tpd_wb_end, tpd_wb_start_local, TPD_WARP_CNT*4);
#endif 

#if (defined(TPD_HAVE_CALIBRATION) && !defined(TPD_CUSTOM_CALIBRATION))
    memcpy(tpd_calmat, tpd_def_calmat_local, 8*4);
    memcpy(tpd_def_calmat, tpd_def_calmat_local, 8*4);	
#endif  
		TPD_DMESG("end %s, %d\n", __FUNCTION__, __LINE__);  
		tpd_type_cap = 1;
    return 0; 
 }

void tp_write_reg0(void)
{
ft5x0x_write_reg(0x8B,0x00);
}
EXPORT_SYMBOL(tp_write_reg0);

void tp_write_reg1(void)
{
ft5x0x_write_reg(0x8B,0x01);
}
EXPORT_SYMBOL(tp_write_reg1);

 static void tpd_resume( struct early_suspend *h )
 {
  //int retval = TPD_OK;
  char data;
 
   TPD_DMESG("TPD wake up\n");
#if defined (CONFIG_SUPPORT_FTS_CTP_UPG)
   	if(1 == atomic_read(&upgrading))
	{
		return;
	}
#endif /* CONFIG_SUPPORT_FTS_CTP_UPG */

#ifdef TPD_CLOSE_POWER_IN_SLEEP	
	hwPowerOn(TPD_POWER_SOURCE,VOL_3300,"TP"); 
#else
	discard_resume_first_eint = KAL_TRUE;
#ifdef TPD_POWER_SOURCE_CUSTOM
    	hwPowerOn(TPD_POWER_SOURCE_CUSTOM, VOL_2800, "TP");
#else
    	hwPowerOn(MT65XX_POWER_LDO_VGP2, VOL_2800, "TP");
#endif
#ifdef TPD_POWER_SOURCE_1800
    	hwPowerOn(TPD_POWER_SOURCE_1800, VOL_1800, "TP");
#endif	
	mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ZERO);  
    msleep(10);  
    mt_set_gpio_mode(GPIO_CTP_RST_PIN, GPIO_CTP_RST_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_CTP_RST_PIN, GPIO_DIR_OUT);
    mt_set_gpio_out(GPIO_CTP_RST_PIN, GPIO_OUT_ONE);
#endif
	msleep(200);//add this line 
   mt_eint_unmask(CUST_EINT_TOUCH_PANEL_NUM);  

//#endif

#ifdef ESD_CHECK	
    	msleep(1);  
	queue_delayed_work(ctp_read_id_workqueue, &ctp_read_id_work,400); //schedule a work for the first detection					
#endif
	
       msleep(20);
	if((i2c_smbus_read_i2c_block_data(ft5336_i2c_client, 0x00, 1, &data))< 0)
	{
		TPD_DMESG("resume I2C transfer error, line: %d\n", __LINE__);
	}

    //if(check_charger_exist()==KAL_TRUE)
    {
		ft5x0x_write_reg(0x8B,0x01);
	}
	tpd_halt = 0;//add this line 
	tpd_up(0,0,0);
	input_sync(tpd->dev);
	TPD_DMESG("TPD wake up done\n");
	 //return retval;
 }

 static void tpd_suspend( struct early_suspend *h )
 {
	// int retval = TPD_OK;
	 static char data = 0x3;
#if defined (CONFIG_SUPPORT_FTS_CTP_UPG)
	if(1 == atomic_read(&upgrading))
	{
		return;
	}
#endif /* CONFIG_SUPPORT_FTS_CTP_UPG */
#ifdef ESD_CHECK	
 	cancel_delayed_work_sync(&ctp_read_id_work);
#endif
	 TPD_DMESG("TPD enter sleep\n");

	tpd_halt = 1; //add this line 
        return;
	 mt_eint_mask(CUST_EINT_TOUCH_PANEL_NUM);
#ifdef TPD_CLOSE_POWER_IN_SLEEP	
	hwPowerDown(TPD_POWER_SOURCE,"TP");
#else
i2c_smbus_write_i2c_block_data(ft5336_i2c_client, 0xA5, 1, &data);  //TP enter sleep mode
#ifdef TPD_POWER_SOURCE_CUSTOM
    	hwPowerDown(TPD_POWER_SOURCE_CUSTOM,  "TP");
#else
    	hwPowerDown(MT65XX_POWER_LDO_VGP2,  "TP");
#endif
#ifdef TPD_POWER_SOURCE_1800
    	hwPowerDown(TPD_POWER_SOURCE_1800,  "TP");
#endif

#endif
        TPD_DMESG("TPD enter sleep done\n");
	 //return retval;
 } 


static ssize_t show_chipinfo(struct device *dev,struct device_attribute *attr, char *buf)
{
	struct i2c_client *client =ft5336_i2c_client;
    unsigned char ver=0;
    unsigned char id=0;
    unsigned char doubleclick = 0;
    unsigned char doubleclickstr[2][10]={" "," (dc)"};
	if(NULL == client)
	{
		printk("i2c client is null!!\n");
		return 0;
	}
	//printk("[TSP]ID_ver=%x, fw_ver=%x\n", ft5x0x_read_ID_ver(), ft5x0x_read_fw_ver());
	//ft5x0x_read_reg(client,TOUCH_FMV_ID,&ver);
	//return sprintf(buf, "[ft5336] ID_ver=%x,fw_ver=%x\n", ft5x0x_read_ID_ver(), ft5x0x_read_fw_ver()); 
	#if defined(FTS_VENDOR_DISTINCT_BY_LCM)
	if(strcmp(lcm_drv->name,LCM_NAME1) == 0
    #if defined(LCM_NAME11)
    || strcmp(lcm_drv->name,LCM_NAME11) == 0
    #endif
    )
	{
		id = 0x5a; // truly TP's ID
	}
	else if(strcmp(lcm_drv->name,LCM_NAME2) == 0)
	{
		id = 0x55; // tianma TP's ID
	}
	#else
	id=ft5x0x_read_ID_ver();
	#endif
	ver=ft5x0x_read_fw_ver();
	doubleclick = ft5x0x_read_doubleclick_flag();
	switch (id)
	{
	}

}

static DEVICE_ATTR(chipinfo, 0664, show_chipinfo, NULL);	//Modify by EminHuang 20120613   0444 -> 0664 [CTS Test]				android.permission.cts.FileSystemPermissionTest#testAllFilesInSysAreNotWritable FAIL

static const struct device_attribute * const ctp_attributes[] = {
	&dev_attr_chipinfo
};


static struct tpd_driver_t tpd_device_driver = {
		 .tpd_device_name = "FT5336",
		 .tpd_local_init = tpd_local_init,
		 .suspend = tpd_suspend,
		 .resume = tpd_resume,
		 .attrs=
		 {
			.attr=ctp_attributes//,*/
		 },
#ifdef TPD_HAVE_BUTTON
		 .tpd_have_button = 1,
#else
		 .tpd_have_button = 0,
#endif		
 };

 

 /* called when loaded into kernel */
 #if defined(CONFIG_SUPPORT_FTS_CTP_UPG)
 static ssize_t tp_test(struct kobject *kobj,
			struct bin_attribute *attr,
			char *buf, loff_t off, size_t count)
{
		uint16_t val;
		printk("tp_test\n");
		if(fts_ctpm_fw_upgrade_with_i_file()!=0){
		TPD_DMESG(TPD_DEVICE " luosen failed to upgrade firmware, line: %d\n", __LINE__);
	}
	return count;
}
static ssize_t tp_read(struct kobject *kobj,
			struct bin_attribute *attr, 
			char *buf, loff_t off, size_t count)
{
	printk("tp_read!!!!!\n");
	int i=300;
	while(i>0)
	{
	mdelay(100);
	i--;
  }
	return count;
}
static struct bin_attribute tp_mode_attr = {
	.attr = {
		.name = "tp",
		.mode = S_IRUGO | S_IWUSR,
	},
	.size = 4,
	.read = tp_read,
	.write = tp_test,
};
#endif
 static int __init tpd_driver_init(void) {
	 printk("MediaTek FT5336 touch panel driver init\n");
#if defined(CONFIG_SUPPORT_FTS_CTP_UPG)	 
	 int ret;
	ret = sysfs_create_bin_file(&(module_kset->kobj), &tp_mode_attr);
	if (ret) {
		printk(KERN_ERR "<CTP> Failed to create sys file\n");
		return -ENOMEM;
	}
#endif	
	   i2c_register_board_info(0, &ft5336_i2c_tpd, 1);
		 if(tpd_driver_add(&tpd_device_driver) < 0)
			 TPD_DMESG("add FT5336 driver failed\n");
	 return 0;
 }
 
 /* should never be called */
 static void __exit tpd_driver_exit(void) {
	 TPD_DMESG("MediaTek FT5336 touch panel driver exit\n");
	 //input_unregister_device(tpd->dev);
	 tpd_driver_remove(&tpd_device_driver);
 }

 module_init(tpd_driver_init);
 module_exit(tpd_driver_exit);

