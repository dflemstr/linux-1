#include "adafruit_rgb_lcd.h"

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/flex_array.h>

#define DRIVERNAME "adafruit_rgb_lcd"

/* When indexed by a 4-bit value, the corresponding element will be
 * the LCD data pins to set in order to write that value to the LCD
 */
static const uint8_t bit_flip[] = {
        0b0000, 0b1000, 0b0100, 0b1100,
        0b0010, 0b1010, 0b0110, 0b1110,
        0b0001, 0b1001, 0b0101, 0b1101,
        0b0011, 0b1011, 0b0111, 0b1111
};

/* TODO: Maybe replace by regular array, since we're only storing
 * pointers...
 */
DEFINE_FLEX_ARRAY(devices, sizeof (device_data *), 256);
static int device_major;
static int device_next_minor;

static struct i2c_device_id driver_idtable[] = {
        { DRIVERNAME, 0 },
        { {0}, 0 }
};

static struct i2c_driver driver = {
        .driver = {
                .owner = THIS_MODULE,
                .name = DRIVERNAME,
        },
        .id_table = driver_idtable,

        .probe = driver_probe,
        .remove = driver_remove,

        .detect = driver_detect,
};

DEVICE_ATTR(backlight, S_IRUGO | S_IWUSR, get_backlight_attr, set_backlight_attr);

static struct attribute *attributes[] = {
        &dev_attr_backlight.attr,
        /* TODO: add attributes for cursor position, custom
         * characters, input direction, scrolling...
         */
        NULL
};

static const struct attribute_group group = {
        .attrs = attributes,
};

static const struct file_operations fops = {
        .owner = THIS_MODULE,
        .read = chardev_read,
        .write = chardev_write,
        .open = chardev_open,
        .release = chardev_release,
};

MODULE_AUTHOR("David Flemström <david.flemstrom@gmail.com>");
MODULE_DESCRIPTION("Adafruit RGB LCD driver");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(i2c, driver_idtable);
module_init (mod_init);
module_exit (mod_exit);

static int __init mod_init (void)
{
        device_next_minor = 0;
        device_major = register_chrdev (0, DRIVERNAME, &fops);

        if (device_major < 0)
        {
                printk (KERN_ALERT "could not register char device\n");
                return device_major;
        }

        printk (KERN_INFO "%s: registered char device with major %d\n",
                DRIVERNAME, device_major);

        return i2c_add_driver (&driver);
}

static void __exit mod_exit (void)
{
        unregister_chrdev (device_major, DRIVERNAME);
        i2c_del_driver (&driver);
}

static int __devinit driver_probe (struct i2c_client *client, const struct i2c_device_id *id)
{
        int status;
        device_data *data;
        reg_value tmp;

        data = (device_data *) kmalloc (sizeof (*data), GFP_KERNEL | __GFP_ZERO);

        if (!data)
        {
                return -ENOMEM;
        }

        i2c_set_clientdata (client, data);

        data->client = client;
        data->number = device_next_minor;

        /* Read the current pin directions */
        status = get_reg_pair (client, MCP23017_IODIR, &data->iodir);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to read pin directions\n");
                goto error;
        }

        /* We want buttons, and initially the LCD data pins, to be inputs */
        tmp.value = 0;
        tmp.lcd_data = 0b1111;
        tmp.buttons = 0b11111;
        status = set_reg_pair (client, MCP23017_IODIR, &data->iodir, tmp);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to initialize pin directions\n");
                goto error;
        }

        /* Get current pullup resistor status */
        status = get_reg_pair (client, MCP23017_GPPU, &data->gppu);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to read pin pullup resistors\n");
                goto error;
        }

        /* Activate pullup resistors for buttons */
        tmp.value = 0;
        tmp.buttons = 0b11111;
        status = set_reg_pair (client, MCP23017_GPPU, &data->gppu, tmp);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to initialize pin pullup resistors\n");
                goto error;
        }

        /* Find out if any pins are currently outputting power (HIGH) */
        status = get_reg_pair (client, MCP23017_GPIO, &data->gpio);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to read pin outputs\n");
                goto error;
        }

        /* Make sure that all pins are LOW */
        tmp.value = 0;
        status = set_reg_pair (client, MCP23017_GPIO, &data->gpio, tmp);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to initialize pin outputs\n");
                goto error;
        }

        /* Do initial poll of input pins just to make sure we can */
        status = get_reg_pair (client, MCP23017_OLAT, &data->olat);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to read pin inputs\n");
                goto error;
        }

        /* We won't write new pin inputs here */

        /* Light up display */
        status = set_backlight (data, LED_COLOR_ON);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to enable LCD backlight\n");
                goto error;
        }

        /* Init sequence 1 */
        status = send_command (data, 0x33);

        if (status < 0)
        {
                dev_err (&client->dev, "interrupted during init sequence step 1\n");
                goto error;
        }

        /* Init sequence 2 */
        status = send_command (data, 0x32);

        if (status < 0)
        {
                dev_err (&client->dev, "interrupted during init sequence step 2\n");
                goto error;
        }

        /* Configure the controller for our particular LCD layout */
        status = send_command (data, LCD_FUNCTIONSET | LCD_4BITMODE | LCD_2LINE | LCD_5x8DOTS);

        if (status < 0)
        {
                dev_err (&client->dev, "could not change LCD function mode\n");
                goto error;
        }

        /* The LCD will be filled with rectangles upon boot, and might
         * contain random data written by other applications
         */
        status = send_command (data, LCD_CLEARDISPLAY);

        if (status < 0)
        {
                dev_err (&client->dev, "could not clear LCD\n");
                goto error;
        }

        /* We want the cursor to automatically move when we write characters */
        status = send_command (data, LCD_ENTRYMODESET | LCD_ENTRYLEFT | LCD_ENTRYSHIFTDECREMENT);

        if (status < 0)
        {
                dev_err (&client->dev, "could not change LCD entry mode\n");
                goto error;
        }

        /* We want the cursor to be visible initially */
        status = send_command (data, LCD_DISPLAYCONTROL | LCD_DISPLAYON | LCD_CURSORON | LCD_BLINKON);

        if (status < 0)
        {
                dev_err (&client->dev, "could not change LCD options\n");
                goto error;
        }

        /* Create SYSFS API */
        status = sysfs_create_group (&client->dev.kobj, &group);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to create sysfs interface\n");
                goto error;
        }

        /* Store device record for char device */
        flex_array_put_ptr (&devices, device_next_minor, data, GFP_KERNEL | __GFP_ZERO);

        dev_info (&client->dev, "registered with minor %d\n", device_next_minor);

        device_next_minor ++;

        return 0;

error:
        kfree (data);
        return status;
}

static int __devexit driver_remove (struct i2c_client *client)
{
        int status;
        int err = 0;

        device_data *data = (device_data *) i2c_get_clientdata (client);

        status = flex_array_clear (&devices, data->number);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to remove minor device number record\n");
                err = status;
        }

        sysfs_remove_group (&client->dev.kobj, &group);

        status = send_command (data, LCD_CLEARDISPLAY);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to clear display\n");
                err = status;
        }

        status = send_command (data, LCD_DISPLAYCONTROL | LCD_DISPLAYOFF);

        if (status < 0)
        {
                dev_err (&client->dev, "could not turn display off\n");
                err = status;
        }

        status = set_backlight (data, LED_COLOR_OFF);

        if (status < 0)
        {
                dev_err (&client->dev, "failed to disable LCD backlight\n");
                err = status;
        }

        kfree (data);

        return err;
}

static int driver_detect (struct i2c_client *client, struct i2c_board_info *info)
{
        /* TODO: find out if there's a way to detect the MCP23017 */
        return 0;
}

static ssize_t chardev_read (
        struct file *file, char __user *user_buf, size_t size, loff_t *offset)
{
        return 0;
}

static ssize_t chardev_write (
        struct file *file, const char __user *user_buf, size_t size, loff_t *offset)
{
        char *buf;
        size_t pos = 0;
        int status = 0;

        device_data *data = (device_data *) file->private_data;

        if (!data)
        {
                return -ENODEV;
        }

        if (size > PAGE_SIZE)
        {
                size = PAGE_SIZE;
        }

        /* TODO: Is it really neccessary to dup this memory?
         * Especially when locking... Maybe for multithreading and
         * when the buffer is free()d in userspace
         */
        buf = memdup_user (user_buf, size);
        while (pos < size)
        {
                status = send_char (data, buf[pos]);

                if (status < 0)
                {
                        goto error;
                }

                pos++;
        }

error:
        kfree (buf);

        if (status < 0)
        {
                return status;
        }
        else
        {
                return pos;
        }
}

static int chardev_open (struct inode *inode, struct file *file)
{
        device_data *data = (device_data *) flex_array_get_ptr (&devices, iminor (inode));

        if (data)
        {
                file->private_data = data;
                return 0;
        }
        else
        {
                return -ENODEV;
        }
}

static int chardev_release (struct inode *inode, struct file *file)
{
        file->private_data = NULL;
        return 0;
}

static int set_backlight (device_data *data, led_color color)
{
        int status;
        reg_value gpio;

        gpio = data->gpio;
        gpio.color = ~(color & 0x7);
        status = set_reg_pair (data->client, MCP23017_GPIO, &data->gpio, gpio);

        if (status < 0)
        {
                dev_err (&data->client->dev, "failed to set color pins\n");
                return status;
        }

        data->backlight_color = color;

        return 0;
}

static ssize_t set_backlight_attr (
        struct device *device, struct device_attribute *attribute,
        const char *str, size_t size)
{
        struct i2c_client *client = to_i2c_client (device);
        device_data *data = (device_data *) i2c_get_clientdata (client);
        led_color value;

        if      (!strncmp ("7", str, 1) || !strncasecmp ("on", str, 2))
        {
                value = LED_COLOR_ON;
        }
        else if (!strncmp ("0", str, 1) || !strncasecmp ("off", str, 3))
        {
                value = LED_COLOR_OFF;
        }
        else if (!strncmp ("1", str, 1) || !strncasecmp ("red", str, 3))
        {
                value = LED_COLOR_RED;
        }
        else if (!strncmp ("2", str, 1) || !strncasecmp ("green", str, 5))
        {
                value = LED_COLOR_GREEN;
        }
        else if (!strncmp ("4", str, 1) || !strncasecmp ("blue", str, 4))
        {
                value = LED_COLOR_BLUE;
        }
        else if (!strncmp ("3", str, 1) || !strncasecmp ("yellow", str, 6))
        {
                value = LED_COLOR_YELLOW;
        }
        else if (!strncmp ("6", str, 1) || !strncasecmp ("teal", str, 4))
        {
                value = LED_COLOR_TEAL;
        }
        else if (!strncmp ("5", str, 1) || !strncasecmp ("violet", str, 6))
        {
                value = LED_COLOR_VIOLET;
        }
        else
        {
               return -EINVAL;
        }

        set_backlight (data, value);
        return size;
}

static ssize_t get_backlight_attr (
        struct device *device, struct device_attribute *attribute, char *str)
{
        struct i2c_client *client = to_i2c_client (device);
        device_data *data = (device_data *) i2c_get_clientdata (client);

        return snprintf (str, PAGE_SIZE, "%d\n", data->backlight_color);
}

static int sync_cursorpos (device_data *data)
{
        uint8_t pos;

        if (data->lcd_col < 0)
        {
                data->lcd_col = 0;
        }
        else if (data->lcd_col > 16)
        {
                data->lcd_col = 16;
        }

        if (!data->lcd_row)
        {
                pos = 0x00;
        }
        else
        {
                pos = 0x40;
        }
        pos += data->lcd_col;

        return send_command (data, LCD_SETDDRAMADDR | pos);
}

static int send_command (device_data *data, uint8_t command)
{
        int status;
        reg_value iodir;

        status = write_data (data, false, command);

        if (status < 0)
        {
                return status;
        }

        /* "Pollable" commands that we need to wait for next
         * time. "Solution:" switch to data input to wait for write
         * next time.
         */
        if (command == LCD_CLEARDISPLAY || command == LCD_RETURNHOME)
        {
                iodir = data->iodir;
                iodir.lcd_data = 0b1111;

                status = set_reg_pair (data->client, MCP23017_IODIR, &data->iodir, iodir);
        }
        return status;
}

static int send_char (device_data *data, uint8_t value)
{
        int status = 0;

        if (data->lcd_escape)
                goto escape;

        switch (value)
        {
        case '\x1B':
                data->lcd_escape = LCD_ESCAPE_SINGLE;
                break;
        case '\n':
                /* Go to beginning of "next" row */
                data->lcd_row = !data->lcd_row;
                data->lcd_col = 0;
                status = sync_cursorpos (data);

                if (status < 0)
                {
                        return status;
                }

                /* Clear the new row */
                while (data->lcd_col < 16)
                {
                        status = write_data (data, true, ' ');
                        if (status < 0)
                        {
                                return status;
                        }
                        data->lcd_col ++;
                }

                /* Go back to the beginning again */
                data->lcd_col = 0;
                status = sync_cursorpos (data);
                break;
        default:
                /* Display scrolling is not implemented yet, so never
                 * write outside of the screen
                 */
                if (data->lcd_col < 16)
                {
                        data->lcd_col += 1;
                        status = write_data (data, true, value);
                }
        }
        return status;

escape:
        switch (value)
        {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
                /* Are we after a '['? */
                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE)
                {
                        /* Increase the currently parsed prefix,
                         * i.e. this might be the second/third digit
                         */
                        data->lcd_esc_pref[data->lcd_esc_pref_pos] =
                                data->lcd_esc_pref[data->lcd_esc_pref_pos] * 10
                                + (value - '0');
                }
                else
                {
                        /* Assume that "\e[0-9]" is some command we
                         * don't understand, and eat the digit
                         */
                        data->lcd_escape = LCD_ESCAPE_NONE;
                }
                break;
        case ';':
                /* If we are after a '[', switch to next prefix */
                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE)
                {
                        /* We support a reasonable number of prefixes */
                        if (data->lcd_esc_pref_pos < LCD_MAX_ESCAPE_PREFIXES - 1)
                        {
                                data->lcd_esc_pref_pos += 1;
                                data->lcd_esc_pref[data->lcd_esc_pref_pos] = 0;
                        }
                }
                else
                {
                        /* We don't understand "\e;", continue */
                        data->lcd_escape = LCD_ESCAPE_NONE;
                }
                break;
        case '[':
                /* Begin a longer escape sequence that might have
                 * prefix number arguments
                 */
                data->lcd_esc_pref_pos = 0;
                data->lcd_esc_pref[0] = 0;
                data->lcd_escape = LCD_ESCAPE_SEQUENCE;
                break;
        case 'E': /* Cursor next line */
        case 'F': /* Cursor previous line */
                data->lcd_col = 0;
                /* Fall through */
        case 'A': /* Cursor up */
        case 'B': /* Cursor down */
                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE &&
                    data->lcd_esc_pref[0] > 0)
                {
                        /* We only have 2 rows, so find out if odd or even */
                        data->lcd_row = data->lcd_row ^ (data->lcd_esc_pref[0] & 1);
                }
                else
                {
                        data->lcd_row = !data->lcd_row;
                }

                status = sync_cursorpos (data);
                data->lcd_escape = LCD_ESCAPE_NONE;
                break;
        case 'C': /* Cursor forward */
                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE &&
                    data->lcd_esc_pref[0] > 0)
                {
                        data->lcd_col += data->lcd_esc_pref[0];
                }
                else
                {
                        data->lcd_col ++;
                }

                status = sync_cursorpos (data);
                data->lcd_escape = LCD_ESCAPE_NONE;
                break;
        case 'D': /* Cursor backward */
                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE &&
                    data->lcd_esc_pref[0] > 0)
                {
                        data->lcd_col -= data->lcd_esc_pref[0];
                }
                else
                {
                        data->lcd_col --;
                }

                status = sync_cursorpos (data);
                data->lcd_escape = LCD_ESCAPE_NONE;
                break;
        case 'G': /* Cursor horizontal attribute */
                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE &&
                    data->lcd_esc_pref[0] > 0)
                {
                        data->lcd_col = data->lcd_esc_pref[0] - 1;

                        if (data->lcd_col > 16)
                        {
                                data->lcd_col = 16;
                        }
                }
                else
                {
                        data->lcd_col = 0;
                }

                status = sync_cursorpos (data);

                data->lcd_escape = LCD_ESCAPE_NONE;
                break;
        case 'f': /* Horizontal and vertical position */
        case 'H': /* Cursor position */

                data->lcd_col = 0;
                data->lcd_row = 0;

                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE &&
                    data->lcd_esc_pref_pos > 0 &&
                    data->lcd_esc_pref[1] > 0)
                {
                        data->lcd_col = data->lcd_esc_pref[1] - 1;
                }

                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE &&
                    data->lcd_esc_pref[0] > 0)
                {
                        data->lcd_row = data->lcd_esc_pref[0] - 1;
                }

                status = sync_cursorpos (data);

                data->lcd_escape = LCD_ESCAPE_NONE;
                break;
        case 'J': /* Erase data */
                /* It makes no sense to say "above or below" the
                 * cursor since the lines are circulated (maybe change
                 * this?), so just clear the screen all the time
                 */
                status = send_command (data, LCD_CLEARDISPLAY);
                data->lcd_escape = LCD_ESCAPE_NONE;
                break;
        case 'K': /* Erase in line */
                if (data->lcd_escape == LCD_ESCAPE_SEQUENCE &&
                    data->lcd_esc_pref[0] == 1)
                {
                        /* Clear to beginning of line */
                        int save_col = data->lcd_col;
                        data->lcd_col = 0;
                        status = sync_cursorpos (data);

                        if (status < 0)
                        {
                                return status;
                        }

                        while (data->lcd_col < save_col)
                        {
                                status = write_data (data, true, ' ');
                                if (status < 0)
                                {
                                        return status;
                                }
                                data->lcd_col ++;
                        }
                }
                else if (data->lcd_escape == LCD_ESCAPE_SEQUENCE &&
                         data->lcd_esc_pref[0] == 2)
                {
                        /* Clear whole line */
                        int save_col = data->lcd_col;
                        data->lcd_col = 0;

                        status = sync_cursorpos (data);

                        if (status < 0)
                        {
                                return status;
                        }

                        while (data->lcd_col < 16)
                        {
                                status = write_data (data, true, ' ');

                                if (status < 0)
                                {
                                        return status;
                                }
                                data->lcd_col ++;
                        }

                        data->lcd_col = save_col;
                        status = sync_cursorpos (data);
                }
                else
                {
                        /* Clear to end of line */
                        int save_col = data->lcd_col;
                        while (data->lcd_col < 16)
                        {
                                status = write_data (data, true, ' ');
                                if (status < 0)
                                {
                                        return status;
                                }
                                data->lcd_col ++;
                        }
                        data->lcd_col = save_col;
                        status = sync_cursorpos (data);
                }
                data->lcd_escape = LCD_ESCAPE_NONE;
                break;
        default:
                /* Can't interpret escape code, so just ignore it */
                data->lcd_escape = LCD_ESCAPE_NONE;
        }
        return status;
}

static int write_data (device_data *data, bool is_char, uint8_t value)
{
        int status;
        reg_value olat;

        wait_for_write (data);

        olat = data->olat;
        olat.lcd_rw = false;
        olat.lcd_rs = is_char;
        olat.lcd_data = bit_flip[value >> 4];
        olat.lcd_enable = true;
        status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat);

        if (status < 0)
        {
                dev_err (&data->client->dev, "failed to update OLAT registers during "
                         "high bit strobe\n");
                return status;
        }

        olat.lcd_data = bit_flip[value & 0xf];
        olat.lcd_enable = false;
        status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat);

        if (status < 0)
        {
                dev_err (&data->client->dev, "failed to update OLAT registers during "
                         "low bit write/high bit strobe end\n");
                return status;
        }

        olat.lcd_enable = true;
        status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat);

        if (status < 0)
        {
                dev_err (&data->client->dev, "failed to update OLAT registers during "
                         "low bit strobe\n");
                return status;
        }

        olat.lcd_enable = false;
        status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat);

        if (status < 0)
        {
                dev_err (&data->client->dev, "failed to update OLAT registers during "
                         "low bit strobe end\n");
                return status;
        }

        return 0;
}

static int wait_for_write (device_data *data)
{
        int status;
        reg_value olat_low;
        reg_value olat_high;
        reg_value iodir;

        unsigned attempts = LCD_WRITE_ATTEMPTS; /* TODO: escape hatch */

        if (data->iodir.lcd_data == 0)
                return 0; /* Already in write mode */

        olat_low = data->olat;
        olat_low.lcd_data = 0;
        olat_low.lcd_enable = false;
        olat_low.lcd_rw = true;
        olat_low.lcd_rs = false;

        olat_high = olat_low;
        olat_high.lcd_enable = true;

        status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat_low);

        if (status < 0)
        {
                dev_err (&data->client->dev, "failed to update GPIO "
                         "registers during wait-for-write\n");
                return status;
        }

        while (--attempts)
        {
                status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat_high);

                if (status < 0)
                {
                        dev_err (&data->client->dev, "failed to high strobe OLAT "
                                 "registers during wait-for-write\n");
                        return status;
                }

                status = get_reg (data->client, MCP23017_GPIO, true, &data->gpio);

                if (status < 0)
                {
                        dev_err (&data->client->dev, "failed to read GPIO "
                                 "registers during wait-for-write\n");
                        return status;
                }

                status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat_low);

                if (status < 0)
                {
                        dev_err (&data->client->dev, "failed to low strobe OLAT "
                                 "registers during wait-for-write\n");
                        return status;
                }

                if (!data->gpio.lcd_data0)
                {
                        /* Not busy any more */
                        break;
                }

                status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat_high);

                if (status < 0)
                {
                        dev_err (&data->client->dev, "failed to high strobe OLAT "
                                 "registers during wait-for-write retry\n");
                        return status;
                }

                status = set_reg_pair (data->client, MCP23017_OLAT, &data->olat, olat_low);

                if (status < 0)
                {
                        dev_err (&data->client->dev, "failed to low strobe OLAT "
                                 "registers during wait-for-write retry\n");
                        return status;
                }
        }

        if (!attempts)
        {
                dev_err (&data->client->dev, "timed out waiting for write, "
                         "continuing anyways...\n");
        }
        else if (LCD_WRITE_ATTEMPTS - attempts > 4)
        {
                dev_warn (&data->client->dev, "waited %d times for write\n",
                          LCD_WRITE_ATTEMPTS - attempts);
        }

        iodir = data->iodir;
        iodir.lcd_data = 0;
        status = set_reg_pair (data->client, MCP23017_IODIR, &data->iodir, iodir);

        if (status < 0)
        {
                dev_err (&data->client->dev, "failed to change IO directions "
                         "during wait-for-write\n");
                return status;
        }

        return 0;
}

static int set_reg_pair (
        struct i2c_client *client, reg reg, reg_value *cache, reg_value value)
{
        int status;

        switch ((cache->value_a != value.value_a) |
                ((cache->value_b != value.value_b) << 1))
        {
        case 0:
                /* Already up to date */
                break;
        case 1:
                status = i2c_smbus_write_byte_data (client, reg, value.value_a);

                if (status < 0)
                {
                        dev_err (&client->dev, "could not write byte to register %x\n", reg);
                        return status;
                }
                break;
        case 2:
                status = i2c_smbus_write_byte_data (client, reg + 1, value.value_b);

                if (status < 0)
                {
                        dev_err (&client->dev, "could not write byte to register %x\n", reg + 1);
                        return status;
                }
                break;
        default:
        case 3:
                status = i2c_smbus_write_word_data (client, reg, value.value);

                if (status < 0)
                {
                        dev_err (&client->dev, "could not write word to register %x\n", reg);
                        return status;
                }
        }

        *cache = value;
        return 0;
}

static int set_reg (
        struct i2c_client *client, reg reg, bool is_b,
        reg_value *cache, reg_value value)
{
        int status = 0;

        if (is_b && cache->value_b != value.value_b)
        {
                status = i2c_smbus_write_byte_data (client, reg + 1, value.value_b);
        }
        else if (!is_b && cache->value_a != value.value_a)
        {
                status = i2c_smbus_write_byte_data (client, reg, value.value_a);
        }

        if (status < 0)
        {
                dev_err (&client->dev, "could not write byte to register %x\n",
                         is_b ? reg + 1 : reg);
                return status;
        }

        if (is_b)
        {
                cache->value_b = value.value_b;
        }
        else
        {
                cache->value_a = value.value_a;
        }

        return 0;
}

static int get_reg_pair (struct i2c_client *client, reg reg, reg_value *value)
{
        int result;

        result = i2c_smbus_read_word_data (client, reg);

        if (result < 0)
        {
                dev_err (&client->dev, "could not read word from register %x\n", reg);
                return result;
        }

        value->value = (uint16_t) result;
        return 0;
}

static int get_reg (
        struct i2c_client *client, reg reg, bool is_b,
        reg_value *value)
{
        int result;

        result = i2c_smbus_read_byte_data (client, is_b ? reg + 1 : reg);

        if (result < 0)
        {
                dev_err (&client->dev, "could not read byte from register %x\n",
                         is_b ? reg + 1 : reg);
                return result;
        }

        if (is_b)
        {
                value->value_b = (uint8_t) result;
        }
        else
        {
                value->value_a = (uint8_t) result;
        }

        return 0;
}
