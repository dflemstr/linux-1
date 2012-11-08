/*
 * Driver the the NXP/Philips PCF2127A RTC (only I2C)
 *
 * Copyright (c) 2011 ECKELMANN AG.
 *
 * Authors:    Matthias Wolf  <m.w...@eckelmann.de>
 *             Torsten Mehnert <t.me...@eckelmann.de>
 *
 * RTC-Datasheet: http://www.nxp.com/documents/data_sheet/PCF2127A.pdf
 *
 * NOTE: This driver only implements the i2c-interface to the RTC.
 * This driver is based on rtc-pcf8363 from Alessandro Zummo.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/i2c.h>
#include <linux/bcd.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/module.h>

/* PCF2127A Registers */
#define PCF2127A_REG_CTL1              0x00 /* Control 1 */
#define PCF2127A_REG_CTL2              0x01 /* Control 2 */
#define PCF2127A_REG_CTL3              0x02 /* Control 3 */

#define PCF2127A_REG_SC                        0x03 /* second value    */
#define PCF2127A_REG_MN                        0x04 /* minute value    */
#define PCF2127A_REG_HR                        0x05 /* hour value      */
#define PCF2127A_REG_DM                        0x06 /* day value       */
#define PCF2127A_REG_DW                        0x07 /* weekday value   */
#define PCF2127A_REG_MO                        0x08 /* month value     */
#define PCF2127A_REG_YR                        0x09 /* year value      */

#define PCF2127A_REG_SC_ALARM          0x0A /* second alarm            */
#define PCF2127A_REG_MN_ALARM          0x0B /* minute alarm            */
#define PCF2127A_REG_HR_ALARM          0x0C /* hour alarm              */
#define PCF2127A_REG_DM_ALARM          0x0D /* day alarm               */
#define PCF2127A_REG_DW_ALARM          0x0E /* weekday alarm           */

#define PCF2127A_REG_CLKO              0x0F /* clock out       */
#define PCF2127A_REG_WD_TMRC           0x10 /* watchdog timer control  */
#define PCF2127A_REG_WD_TMVAL          0x11 /* watchdog timer value    */

#define PCF2127A_REG_TSCTL             0x12 /* timestamp control       */
#define PCF2127A_REG_SC_TS             0x13 /* second timestamp        */
#define PCF2127A_REG_MN_TS             0x14 /* minute timestamp        */
#define PCF2127A_REG_HR_TS             0x15 /* hour timestamp          */
#define PCF2127A_REG_DM_TS             0x16 /* day timestamp           */
#define PCF2127A_REG_MO_TS             0x17 /* month timestamp         */
#define PCF2127A_REG_YR_TS             0x18 /* year timestamp          */
#define PCF2127A_REG_AGI_OFF           0x19 /* aging offset            */
#define PCF2127A_REG_RAM_MSB           0x1A /* RAM address MSB         */
#define PCF2127A_REG_RAM_LSB           0x1B /* RAM address LSB         */
#define PCF2127A_REG_RAM_WCMD          0x1C /* RAM write command       */
#define PCF2127A_REG_RAM_RCMD          0x1D /* RAM read command        */

/* Bits in PCF2127A_REG_CTL1 */
#define PCF2127A_BIT_SI                        0x01 /* second interrupt        */
#define PCF2127A_BIT_MI                        0x02 /* minute interrupt        */
#define PCF2127A_BIT_H_24_12           0x04 /* 12/24 hour mode         */
#define PCF2127A_BIT_POR_OVRD          0x08 /* POR override            */
#define PCF2127A_BIT_TSF1              0x10 /* timestamp flag 1        */
#define PCF2127A_BIT_STOP              0x20 /* stop bit                */
#define PCF2127A_BIT_EXTTEST           0x80 /* external clock test mode*/

/* Bits in PCF2127A_REG_CTL2 */
#define PCF2127A_BIT_CDTIE             0x01 /* countdown timer flag    */
#define PCF2127A_BIT_AIE               0x02 /* alarm interrupt enable  */
#define PCF2127A_BIT_TSIE              0x04 /* timestamp int. enable   */
#define PCF2127A_BIT_CDTF              0x08 /* countdown timer flag    */
#define PCF2127A_BIT_AF                        0x10 /* alarm flag              */
#define PCF2127A_BIT_TSF2              0x20 /* timestamp flag 2        */
#define PCF2127A_BIT_WDTF              0x40 /* watchdog timer flag     */
#define PCF2127A_BIT_MSF               0x80 /* minute/second flag      */

/* Bits in PCF2127A_REG_CTL3 */
#define PCF2127A_BIT_BLIE              0x01 /* battery low Interrupt enable */
#define PCF2127A_BIT_BIE               0x02 /* battery Switch int. enable */
#define PCF2127A_BIT_BLF               0x04 /* battery low flag        */
#define PCF2127A_BIT_BF                        0x08 /* battery switch flag     */
#define PCF2127A_BIT_BTSE              0x10 /* battery timestamp enable */
#define PCF2127A_BIT_BLDOFF            0x20 /* battery low detection off */
#define PCF2127A_BIT_BSM               0x40 /* battery switch mode     */
#define PCF2127A_BIT_BSOFF             0x80 /* battery switch off      */

static struct i2c_driver pcf2127a_driver;

/*
 * In the routines that deal directly with the pcf2127a hardware, we use
 * rtc_time -- month 0-11, hour 0-23, yr = calendar year-epoch.
 */
static
int pcf2127a_get_datetime(struct i2c_client *client, struct rtc_time *tm)
{
       unsigned char buf[10] = { PCF2127A_REG_CTL1 };

       /*
       * Note: We can't use i2c_transfer to send/recv multiple i2c_msg's
       * to/from pcf2127a at once. The reason is that the pcf2127a doesn't
       * handle (repeated START)-Conditions correctly.
       *
       * PCF2127A Manual, Section 9.2.2 START and STOP conditions:
       * "Remark: For the PCF2127A a repeated START is not allowed. Therefore
       * a STOP has to be released before the next START."
       */

       /* setup read ptr */
       if (!i2c_master_send(client, buf, 1)) {
               dev_err(&client->dev, "%s: write error\n", __func__);
               return -EIO;
       }

       /* read status + date */
       if (!i2c_master_recv(client, buf, 10)) {
               dev_err(&client->dev, "%s: read error\n", __func__);
               return -EIO;
       }

       if (buf[PCF2127A_REG_CTL3] & PCF2127A_BIT_BLF)
               dev_info(&client->dev,
                       "low battery detected, date/time is not reliable.\n");

#ifdef DEBUG
       unsigned char bf_buf[2];
       if (buf[PCF2127A_REG_CTL3] & PCF2127A_BIT_BF) {
               dev_info(&client->dev,
                       "battery switch-over detected - reset BF flag.\n");

               bf_buf[0] = PCF2127A_REG_CTL3;
               bf_buf[1] = buf[PCF2127A_REG_CTL3] & ~PCF2127A_BIT_BF;
               if (!i2c_master_send(client, bf_buf, 2)) {
                               dev_err(&client->dev,
                                       "%s: write error\n", __func__);
                               return -EIO;
                       }
               }
#endif

       dev_dbg(&client->dev,
               "%s: raw data is ctl1=%02x, ctl2=%02x, ctl3=%02x, sec=%02x, "
               "min=%02x, hour=%02x, day=%02x, wday=%02x, month=%02x, "
               "year=%02x\n",
               __func__,
               buf[0], buf[1], buf[2], buf[3],
               buf[4], buf[5], buf[6], buf[7],
               buf[8], buf[9]);

       tm->tm_sec = bcd2bin(buf[PCF2127A_REG_SC] & 0x7F);
       tm->tm_min = bcd2bin(buf[PCF2127A_REG_MN] & 0x7F);
       tm->tm_hour = bcd2bin(buf[PCF2127A_REG_HR] & 0x3F); /* rtc hr 0-23 */
       tm->tm_mday = bcd2bin(buf[PCF2127A_REG_DM] & 0x3F);
       tm->tm_wday = buf[PCF2127A_REG_DW] & 0x07;
       tm->tm_mon = bcd2bin(buf[PCF2127A_REG_MO] & 0x1F) - 1; /* rtc m 1-12 */
       tm->tm_year = bcd2bin(buf[PCF2127A_REG_YR]);

       if (tm->tm_year < 70)
               tm->tm_year += 100;     /* assume we are in 1970...2069 */

       /*
        * the clock can give out invalid datetime, but we cannot return
        * -EINVAL otherwise hwclock will refuse to set the time on bootup.
        */
       if (rtc_valid_tm(tm) < 0)
               dev_err(&client->dev, "retrieved date/time is not valid.\n");

       return 0;
}

static
int pcf2127a_set_datetime(struct i2c_client *client, struct rtc_time *tm)
{
       int i, err;
       unsigned char msgdata[2];       /* send data*/
       unsigned char buf[10];          /* Index 0 - 9 */

       /* hours, minutes and seconds */
       buf[PCF2127A_REG_SC] = bin2bcd(tm->tm_sec);
       buf[PCF2127A_REG_MN] = bin2bcd(tm->tm_min);
       buf[PCF2127A_REG_HR] = bin2bcd(tm->tm_hour);

       /* day of month */
       buf[PCF2127A_REG_DM] = bin2bcd(tm->tm_mday);

       /* weekday */
       buf[PCF2127A_REG_DW] = tm->tm_wday & 0x07;

       /* month, 1 - 12 */
       buf[PCF2127A_REG_MO] = bin2bcd(tm->tm_mon + 1);

       /* year */
       buf[PCF2127A_REG_YR] = bin2bcd(tm->tm_year % 100);

       /* write register's data */
       for (i = 0; i < 7; i++) {
               msgdata[0] = PCF2127A_REG_SC + i;
               msgdata[1] = buf[PCF2127A_REG_SC + i];
               err = i2c_master_send(client, msgdata, sizeof(msgdata));
               if (err != sizeof(msgdata)) {
                       dev_err(&client->dev,
                               "%s: err=%d addr=%02x, data=%02x\n",
                               __func__, err, msgdata[0], msgdata[1]);
                       return -EIO;
               }
       }
       return 0;
}

/*---------------------------------------------------------------------------*/

static int pcf2127a_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
       return pcf2127a_get_datetime(to_i2c_client(dev), tm);
}

static int pcf2127a_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
       return pcf2127a_set_datetime(to_i2c_client(dev), tm);
}

static const struct rtc_class_ops pcf2127a_rtc_ops = {
       .read_time      = pcf2127a_rtc_read_time,
       .set_time       = pcf2127a_rtc_set_time,
};

/*---------------------------------------------------------------------------*/

static int pcf2127a_probe(struct i2c_client *client,
                               const struct i2c_device_id *id)
{
       struct rtc_device *pcf2127a_device;

       if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
               return -ENODEV;

       dev_info(&client->dev, "chip found\n");

       pcf2127a_device = rtc_device_register(pcf2127a_driver.driver.name,
                               &client->dev, &pcf2127a_rtc_ops, THIS_MODULE);

       if (IS_ERR(pcf2127a_device))
               return PTR_ERR(pcf2127a_device);

       i2c_set_clientdata(client, pcf2127a_device);

       return 0;
}

/*---------------------------------------------------------------------------*/

static int pcf2127a_remove(struct i2c_client *client)
{
       struct rtc_device *pcf2127a_device = i2c_get_clientdata(client);

       if (pcf2127a_device)
               rtc_device_unregister(pcf2127a_device);

       return 0;
}

static const struct i2c_device_id pcf2127a_id[] = {
       /*
        * This driver should also work with the pcf2127, but
        * I can't prove it.
        */
       { "pcf2127a", 0 },
       { }
};
MODULE_DEVICE_TABLE(i2c, pcf2127a_id);

static struct i2c_driver pcf2127a_driver = {
       .driver         = {
               .name   = "rtc-pcf2127a",
       },
       .probe          = pcf2127a_probe,
       .remove         = pcf2127a_remove,
       .id_table       = pcf2127a_id,
};

static int __init pcf2127a_init(void)
{
       return i2c_add_driver(&pcf2127a_driver);
}
module_init(pcf2127a_init);

static void __exit pcf2127a_exit(void)
{
       i2c_del_driver(&pcf2127a_driver);
}
module_exit(pcf2127a_exit);

MODULE_AUTHOR("ECKELMANN AG");
MODULE_DESCRIPTION("NXP/Philips PCF2127A RTC (I2C) driver");
MODULE_LICENSE("GPL");
