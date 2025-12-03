#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/i2c.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/dmi.h>
#include "pddf_psu_defs.h"

ssize_t pddf_get_custom_psu_model_name(struct device *dev, struct device_attribute *da, char *buf);
ssize_t pddf_get_custom_psu_serial_num(struct device *dev, struct device_attribute *da, char *buf);
ssize_t pddf_get_custom_psu_fan_direction(struct device *dev, struct device_attribute *da, char *buf);
extern PSU_SYSFS_ATTR_DATA access_psu_v_out;
extern PSU_SYSFS_ATTR_DATA access_psu_model_name;
extern PSU_SYSFS_ATTR_DATA access_psu_serial_num;
extern PSU_SYSFS_ATTR_DATA access_psu_fan_dir;

#define MAX_MODEL_NAME          16
#define MAX_SERIAL_NUMBER       19
#define MAX_MODEL_OPT            7
#define MAX_FAN_DIR              3

enum psu_type {
    PSU_TYPE_AC_D850AB_5A,
    PSU_TYPE_AC_D850AB_5B,
    PSU_TYPE_AC_YM2851F,
    PSU_TYPE_DC_YM2851J,
    PSU_TYPE_DC_G1441_0850WNB,
};

struct model_name_info {
    enum psu_type type;
    u8 offset;
    u8 length;
    u8 chk_length;
    char* model_name;
};

struct serial_number_info {
    enum psu_type type;
    u8 offset;
    u8 length;
    u8 chk_length;
    char* serial_number;
};

struct model_name_info models[] = {
    { PSU_TYPE_AC_D850AB_5A, 0x15, 10, 10, "D850AB-5 A" },
    { PSU_TYPE_AC_D850AB_5B, 0x15, 10, 10, "D850AB-5 B" },
    { PSU_TYPE_AC_YM2851F, 0x20, 13, 8, "YM-2851F" },
    { PSU_TYPE_DC_YM2851J, 0x20, 13, 8, "YM-2851J" },
    { PSU_TYPE_DC_G1441_0850WNB, 0x15, 13, 13, "G1441-0850WNB" },
};

struct serial_number_info serials[] = {
    { PSU_TYPE_AC_D850AB_5A, 0x2e, 11, 11, "D850AB-5 A" },
    { PSU_TYPE_AC_D850AB_5B, 0x2e, 11, 11, "D850AB-5 B" },
    { PSU_TYPE_AC_YM2851F, 0x35, 19, 19, "YM-2851F" },
    { PSU_TYPE_DC_YM2851J, 0x35, 19, 19, "YM-2851J" },
    { PSU_TYPE_DC_G1441_0850WNB, 0x3d, 19, 19, "G1441-0850WNB" },
};

struct pddf_psu_data {    
    char model_name[MAX_MODEL_NAME+1];
    char serial_number[MAX_SERIAL_NUMBER+1];
    char model_opt[MAX_MODEL_OPT+1];
    char fan_dir[MAX_FAN_DIR+1];
};


static int pddf_psu_read_block(struct i2c_client *client, u8 command, u8 *data,
                                     int data_len)
{
    int result = 0;
    int retry_count = 10;

    while (retry_count) {
        retry_count--;

        result = i2c_smbus_read_i2c_block_data(client, command, data_len, data);

        if (unlikely(result < 0)) {
            msleep(10);
            continue;
        }

        if (unlikely(result != data_len)) {
            result = -EIO;
            msleep(10);
            continue;
        }

        result = 0;
        break;
    }

    return result;
}

static int pddf_psu_read_byte(struct i2c_client *client, u8 reg)
{
    return i2c_smbus_read_byte_data(client, reg);
}

static int two_complement_to_int(u16 data, u8 valid_bit, int mask)
{
	u16  valid_data  = data & mask;
	bool is_negative = valid_data >> (valid_bit - 1);

	return is_negative ? (-(((~valid_data) & mask) + 1)) : valid_data;
}

static u8 psu_get_vout_mode(struct i2c_client *client)
{
	u8 status = 0, retry = 10;
	uint8_t offset = 0x20; // VOUT_MODE

	while (retry) {
		status = i2c_smbus_read_byte_data((struct i2c_client *)client, offset);
		if (unlikely(status < 0)) {
			msleep(60);
			retry--;
			continue;
		}
		break;
	}

	if (status < 0)
	{
		printk(KERN_ERR "%s: Get PSU Vout mode failed\n", __func__);
		return 0;
	}
	else
	{
		/*printk(KERN_ERR "%s: vout_mode reg value 0x%x\n", __func__, status);*/
		return status;
	}
}

static u16 psu_get_v_out(struct i2c_client *client)
{
	u16 status = 0, retry = 10;
	uint8_t offset = 0x8b; // READ_VOUT

	while (retry) {
		status = i2c_smbus_read_word_data((struct i2c_client *)client, offset);
		if (unlikely(status < 0)) {
			msleep(60);
			retry--;
			continue;
		}
		break;
	}

	if (status < 0)
	{
		printk(KERN_ERR "%s: Get PSU Vout failed\n", __func__);
		return 0;
	}
	else
	{
		/*printk(KERN_ERR "%s: vout reg value 0x%x\n", __func__, status);*/
		return status;
	}
}

ssize_t pddf_show_custom_psu_v_out(struct device *dev, struct device_attribute *da, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct sensor_device_attribute *attr = to_sensor_dev_attr(da);
	int exponent, mantissa;
	int multiplier = 1000;
    struct pddf_psu_data data;
    int status, length;
    u8 data_len, command;
    u16 value;

    memset(data.model_name, 0, sizeof(data.model_name));

    /* Read model_name */
    command = 0x9a;
    length  = 1;
    /* Read first byte to determine the length of data */
    status = pddf_psu_read_block(client, command, &data_len, length);
    if (status < 0) 
    {
        dev_dbg(&client->dev, "reg %d, err %d\n", command, status);
        return status;
    }
    status = pddf_psu_read_block(client, command, data.model_name, data_len+1);
    if (status < 0) 
    {
        dev_dbg(&client->dev, "reg %d, err %d\n", command, status);
        return status;
    }

    if ((data_len+1) >= (ARRAY_SIZE(data.model_name)-1))
    {            
        data.model_name[ARRAY_SIZE(data.model_name)-1] = '\0';
    }
    else
        data.model_name[data_len+1] = '\0';


    value = psu_get_v_out(client);
    /*linear16*/
    if( (!strncmp("DPS-850AB-5 B", data.model_name + 1, strlen("DPS-850AB-5 B"))) ||
        (!strncmp("YM-2851J", data.model_name + 1, strlen("YM-2851J"))) ||
        (!strncmp("G1441-0850WNB", data.model_name + 1, strlen("G1441-0850WNB"))) ) 
    {
        
        u8 vout_mode = psu_get_vout_mode(client);

        if ((vout_mode >> 5) == 0)
            exponent = two_complement_to_int(vout_mode & 0x1f, 5, 0x1f);
        else
        {
            /*printk(KERN_ERR "%s: Only support linear mode for vout mode\n", __func__);*/
            exponent = 0;
        }
        mantissa = value;
    }
    /*linear11*/
    else
    {
        exponent = two_complement_to_int(value >> 11, 5, 0x1f);
        mantissa = two_complement_to_int(value & 0x7ff, 11, 0x7ff);            
    }

    if (exponent >= 0)
        return sprintf(buf, "%d\n", (mantissa << exponent) * multiplier);
    else
        return sprintf(buf, "%d\n", (mantissa * multiplier) / (1 << -exponent));
}

ssize_t pddf_get_custom_psu_serial_num(struct device *dev, struct device_attribute *da, char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct pddf_psu_data data;
    int i, status;

    for (i = 0; i < ARRAY_SIZE(models); i++) {
        memset(data.serial_number, 0, sizeof(data.serial_number));

        status = pddf_psu_read_block(client, models[i].offset,
                                           data.model_name, models[i].length);
        if (status < 0) {
            data.model_name[0] = '\0';
            dev_dbg(&client->dev, "unable to read model name from (0x%x) offset(0x%x)\n",
                                  client->addr, models[i].offset);
            return status;
        }
        else {
            data.model_name[models[i].length] = '\0';
        }

        /* Determine if the model name is known, if not, read next index
         */
        if (strncmp(data.model_name, models[i].model_name, models[i].chk_length) == 0) {

            status = pddf_psu_read_block(client, serials[i].offset,
                                      data.serial_number, serials[i].length);
            if (status < 0) {
                data.serial_number[0] = '\0';
                dev_dbg(&client->dev, "unable to read serial num from (0x%x) offset(0x%x)\n",
                                  client->addr, serials[i].offset);
                return status;
            }
            else {
                data.serial_number[serials[i].length] = '\0';
	            return sprintf(buf, "%s\n", data.serial_number);
            }
                        
            return 0;
        }
        else {
            data.serial_number[0] = '\0';
        }
    }

    return -ENODATA;

	
}

ssize_t pddf_get_custom_psu_model_name(struct device *dev, struct device_attribute *da, char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct pddf_psu_data data;
    int status, length;
    u8 data_len, command;

    memset(data.model_name, 0, sizeof(data.model_name));

    /* Read model_name */
    command = 0x9a;
    length  = 1;
    /* Read first byte to determine the length of data */
    status = pddf_psu_read_block(client, command, &data_len, length);
    if (status < 0) 
    {
        dev_dbg(&client->dev, "reg %d, err %d\n", command, status);
        return status;
    }
    status = pddf_psu_read_block(client, command, data.model_name, data_len+1);
    if (status < 0) 
    {
        dev_dbg(&client->dev, "reg %d, err %d\n", command, status);
        return status;
    }

    if ((data_len+1) >= (ARRAY_SIZE(data.model_name)-1))
    {            
        data.model_name[ARRAY_SIZE(data.model_name)-1] = '\0';
    }
    else
        data.model_name[data_len+1] = '\0';

    if ( (!strncmp("YM-2851F", data.model_name+1, strlen("YM-2851F"))) ||
            (!strncmp("YM-2851J", data.model_name+1, strlen("YM-2851J"))) )
    {
        /* Read model opt */
        command = 0xd0;
        length = 1;

        status = pddf_psu_read_block(client, command, &data_len, length);
        if (status < 0) {
            dev_dbg(&client->dev, "unable to read model option length from (0x%x) offset(0x%x)\n",
                                    client->addr, command);
            return status;
        }

        status = pddf_psu_read_block(client, command, data.model_opt, data_len+1);
        if (status < 0) {
            dev_dbg(&client->dev, "unable to read model option from (0x%x) offset(0x%x)\n",
                                    client->addr, command);
            return status;
        }
        else{
            data.model_opt[data_len+1] = '\0';
        }
        return sprintf(buf, "%s%s\n", data.model_name+1,data.model_opt+1);
    }
    else
    {
        return sprintf(buf, "%s\n", data.model_name+1);
    }

}

ssize_t pddf_get_custom_psu_fan_direction(struct device *dev, struct device_attribute *da, char *buf)
{
    struct i2c_client *client = to_i2c_client(dev);
    struct pddf_psu_data data;
    int status, length;
    u8 data_len, command;

    memset(data.model_name, 0, sizeof(data.model_name));

    /* Read model_name */
    command = 0x9a;
    length  = 1;
    /* Read first byte to determine the length of data */
    status = pddf_psu_read_block(client, command, &data_len, length);
    if (status < 0) 
    {
        dev_dbg(&client->dev, "reg %d, err %d\n", command, status);
        return status;
    }
    status = pddf_psu_read_block(client, command, data.model_name, data_len+1);
    if (status < 0) 
    {
        dev_dbg(&client->dev, "reg %d, err %d\n", command, status);
        return status;
    }

    if ((data_len+1) >= (ARRAY_SIZE(data.model_name)-1))
    {            
        data.model_name[ARRAY_SIZE(data.model_name)-1] = '\0';
    }
    else
        data.model_name[data_len+1] = '\0';

    /* Read fan direction */
    command = 0xC3;
    memset(data.fan_dir, 0, sizeof(data.fan_dir));
    if (!strncmp("G1441-0850WNB", data.model_name+1, strlen("G1441-0850WNB")) ){
       memcpy(data.fan_dir, "F2B", 3);
    }
    else
    {

        status = pddf_psu_read_byte(client, command);

        if (status < 0)
        {
            dev_dbg(&client->dev, "reg %d, err %d\n", command, status);
            return status;
        }
        else
        {
            u16 B2F_flag = 0x01; /*BIT[4:3]=01: AFI*/
            u16 F2B_flag = 0x02; /*BIT[4:3]=10: AFO*/
            if( (status >> 3)& B2F_flag)
            {
                memcpy(data.fan_dir, "B2F", 3);
            }
            else if( (status >> 3)& F2B_flag)
            {
                memcpy(data.fan_dir, "F2B", 3);
            }
            else
            {
                char fan_dir[5];
                status = pddf_psu_read_block(client, command, fan_dir, ARRAY_SIZE(fan_dir)-1);
                if (status == 0) 
                {
                    strncpy(data.fan_dir, fan_dir+1, ARRAY_SIZE(data.fan_dir)-1);
                    data.fan_dir[ARRAY_SIZE(data.fan_dir)-1] = '\0';
                }
            }
        }
    }
    return sprintf(buf, "%s\n", data.fan_dir);

}

static int __init pddf_custom_psu_init(void)
{
	access_psu_v_out.show = pddf_show_custom_psu_v_out;
	access_psu_v_out.do_get = NULL;

    access_psu_serial_num.show = pddf_get_custom_psu_serial_num;
    access_psu_serial_num.do_get = NULL;
	
    access_psu_model_name.show = pddf_get_custom_psu_model_name;
    access_psu_model_name.do_get = NULL;
	
    access_psu_fan_dir.show = pddf_get_custom_psu_fan_direction;
    access_psu_fan_dir.do_get = NULL;
	return 0;
}

static void __exit pddf_custom_psu_exit(void)
{
	printk(KERN_ERR "pddf_custom_psu_exit\n");
	return;
}

MODULE_AUTHOR("Broadcom");
MODULE_DESCRIPTION("pddf custom psu api");
MODULE_LICENSE("GPL");

module_init(pddf_custom_psu_init);
module_exit(pddf_custom_psu_exit);

