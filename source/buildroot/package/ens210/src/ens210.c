// SPDX-License-Identifier: GPL-2.0
/* Minimal IIO driver for ScioSense ENS210 temperature + relative humidity.
 * No mainline driver exists; this covers single-shot reads for the Toon. */
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>

#define ENS210_REG_PART_ID   0x00
#define ENS210_REG_SYS_CTRL  0x10
#define ENS210_REG_SYS_STAT  0x11
#define ENS210_REG_SENS_RUN  0x21
#define ENS210_REG_SENS_START 0x22
#define ENS210_REG_T_VAL     0x30
#define ENS210_REG_H_VAL     0x33
#define ENS210_PART_ID       0x0210

struct ens210 { struct i2c_client *client; struct mutex lock; };

static int ens210_read_meas(struct ens210 *d, bool temp, int *val)
{
	struct i2c_client *c = d->client;
	u8 buf[3];
	int ret, raw;
	/* single-shot: SENS_RUN=0 (single), start both T+H */
	i2c_smbus_write_byte_data(c, ENS210_REG_SENS_RUN, 0x00);
	ret = i2c_smbus_write_byte_data(c, ENS210_REG_SENS_START, 0x03);
	if (ret < 0) return ret;
	msleep(140);
	ret = i2c_smbus_read_i2c_block_data(c,
		temp ? ENS210_REG_T_VAL : ENS210_REG_H_VAL, 3, buf);
	if (ret < 0) return ret;
	if (!(buf[2] & 0x01)) return -EIO; /* valid bit */
	raw = buf[0] | (buf[1] << 8);
	*val = raw;
	return 0;
}

static int ens210_read_raw(struct iio_dev *idev,
	struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	struct ens210 *d = iio_priv(idev);
	int raw, ret;
	if (mask != IIO_CHAN_INFO_PROCESSED) return -EINVAL;
	mutex_lock(&d->lock);
	ret = ens210_read_meas(d, chan->type == IIO_TEMP, &raw);
	mutex_unlock(&d->lock);
	if (ret) return ret;
	if (chan->type == IIO_TEMP)        /* K/64 -> milli-degC */
		*val = (raw * 1000 / 64) - 273150;
	else                               /* /512 -> milli-%RH */
		*val = raw * 1000 / 512;
	return IIO_VAL_INT;
}

static const struct iio_info ens210_info = { .read_raw = ens210_read_raw };
static const struct iio_chan_spec ens210_channels[] = {
	{ .type = IIO_TEMP, .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) },
	{ .type = IIO_HUMIDITYRELATIVE, .info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED) },
};

static int ens210_probe(struct i2c_client *client)
{
	struct iio_dev *idev;
	struct ens210 *d;
	u8 id[2];
	int ret;
	ret = i2c_smbus_write_byte_data(client, ENS210_REG_SYS_CTRL, 0x80); /* reset */
	if (ret < 0) return ret;
	msleep(5);
	i2c_smbus_write_byte_data(client, ENS210_REG_SYS_CTRL, 0x00); /* active */
	msleep(5);
	ret = i2c_smbus_read_i2c_block_data(client, ENS210_REG_PART_ID, 2, id);
	if (ret < 0) return ret;
	if ((id[0] | (id[1] << 8)) != ENS210_PART_ID)
		dev_warn(&client->dev, "unexpected part id 0x%04x\n", id[0]|(id[1]<<8));
	idev = devm_iio_device_alloc(&client->dev, sizeof(*d));
	if (!idev) return -ENOMEM;
	d = iio_priv(idev);
	d->client = client;
	mutex_init(&d->lock);
	idev->name = "ens210";
	idev->info = &ens210_info;
	idev->modes = INDIO_DIRECT_MODE;
	idev->channels = ens210_channels;
	idev->num_channels = ARRAY_SIZE(ens210_channels);
	return devm_iio_device_register(&client->dev, idev);
}

static const struct i2c_device_id ens210_id[] = { { "ens210", 0 }, {} };
MODULE_DEVICE_TABLE(i2c, ens210_id);
static const struct of_device_id ens210_of[] = { { .compatible = "ams,ens210" }, {} };
MODULE_DEVICE_TABLE(of, ens210_of);

static struct i2c_driver ens210_driver = {
	.driver = { .name = "ens210", .of_match_table = ens210_of },
	.probe = ens210_probe,
	.id_table = ens210_id,
};
module_i2c_driver(ens210_driver);
MODULE_DESCRIPTION("ScioSense ENS210 temp/humidity IIO driver");
MODULE_LICENSE("GPL");
