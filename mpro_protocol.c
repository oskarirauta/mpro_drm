// SPDX-License-Identifier: GPL-2.0
/*
 * mpro_protocol.c — synchronous control-message protocol.
 *
 * Two flavors:
 *
 *  - mpro_make_request / mpro_request_u32: query/response using the
 *    three-step request protocol (send cmd -> read status -> read payload).
 *    Used for probe-time queries: screen_id, version, device id.
 *
 *  - mpro_send_command: single-shot vendor command (no response).
 *    Used for backlight, sleep-mode wakeup, flip/mirror, etc.
 *
 * All calls are mutually exclusive via mpro->lock.
 */

#include <linux/module.h>
#include <linux/delay.h>

#include "mpro_internal.h"
#include "mpro.h"

int mpro_make_request(struct mpro_device *mpro,
		      const void *tx, size_t txlen, void *rx, size_t rxlen)
{
	u8 *buf = mpro->xfer_buf;
	int ret;

	mutex_lock(&mpro->lock);

	/* Send request */
	ret = usb_control_msg_send(mpro->udev, 0,
				   MPRO_REQ_CMD, MPRO_USB_OUT, 0, 0,
				   (void *)tx, txlen,
				   MPRO_USB_TIMEOUT_MS, GFP_KERNEL);
	if (ret) {
		dev_err(&mpro->intf->dev,
			"failed to send request %*phC\n", (int)txlen, tx);
		goto out;
	}

	if (!rx || rxlen == 0)
		goto out;

	/* Let device prepare status byte */
	if (mpro->request_delay)
		msleep(mpro->request_delay);

	/* Read status byte */
	ret = usb_control_msg_recv(mpro->udev, 0,
				   MPRO_REQ_STATUS, MPRO_USB_IN, 0, 0,
				   buf, 1, MPRO_USB_TIMEOUT_MS, GFP_KERNEL);
	if (ret) {
		dev_err(&mpro->intf->dev,
			"failed to read status byte for request %*phC\n",
			(int)txlen, tx);
		goto out;
	}

	/*
	 * Status byte semantics are undocumented; the previous "if !=
	 * 0x51, error" check was wrong on some firmware versions. We
	 * skip validation and accept whatever the device returns.
	 */

	/* Let device settle before payload read */
	if (mpro->request_delay)
		msleep(mpro->request_delay);

	/* Read payload (with leading status byte we discard) */
	ret = usb_control_msg_recv(mpro->udev, 0,
				   MPRO_REQ_READ, MPRO_USB_IN, 0, 0,
				   buf, rxlen + 1,
				   MPRO_USB_TIMEOUT_MS, GFP_KERNEL);
	if (ret) {
		dev_err(&mpro->intf->dev,
			"failed to read payload for request %*phC\n",
			(int)txlen, tx);
		goto out;
	}

	memcpy(rx, buf + 1, rxlen);
	ret = 0;

out:
	mutex_unlock(&mpro->lock);
	return ret;
}

EXPORT_SYMBOL_GPL(mpro_make_request);

int mpro_request_u32(struct mpro_device *mpro,
		     const void *tx, size_t txlen, u32 *val)
{
	__le32 tmp;
	int ret;

	if (!val)
		return -EINVAL;

	ret = mpro_make_request(mpro, tx, txlen, &tmp, sizeof(tmp));
	if (ret)
		return ret;

	*val = le32_to_cpu(tmp);
	return 0;
}

EXPORT_SYMBOL_GPL(mpro_request_u32);

int mpro_send_command(struct mpro_device *mpro, const void *cmd, size_t cmd_len)
{
	int ret;

	if (!mpro || !mpro->udev || !cmd)
		return -EINVAL;

	mutex_lock(&mpro->lock);
	ret = usb_control_msg_send(mpro->udev, 0,
				   MPRO_CMD, MPRO_USB_OUT, 0, 0,
				   (void *)cmd, cmd_len,
				   MPRO_USB_TIMEOUT_MS, GFP_KERNEL);
	mutex_unlock(&mpro->lock);
	return ret;
}

EXPORT_SYMBOL_GPL(mpro_send_command);

static int mpro_get_firmware_string(struct mpro_device *mpro,
				    char *out, size_t outlen)
{
	u8 *buf = mpro->xfer_buf;
	int ret;

	mutex_lock(&mpro->lock);

	/* Vaihe 1: alustus 0xB5 OUT */
	memcpy(buf, "\x51\x02\x04\x1f\xf8", 5);
	ret = usb_control_msg(mpro->udev, usb_sndctrlpipe(mpro->udev, 0),
			      0xB5, 0x40, 0, 0, buf, 5, MPRO_USB_TIMEOUT_MS);
	if (ret < 0)
		goto out;

	if (mpro->request_delay)
		msleep(mpro->request_delay);

	/* Vaihe 2: status 0xB6 IN, len=1 */
	ret = usb_control_msg(mpro->udev, usb_rcvctrlpipe(mpro->udev, 0),
			      0xB6, 0xC0, 0, 0, buf, 1, MPRO_USB_TIMEOUT_MS);
	if (ret < 0)
		goto out;

	if (mpro->request_delay)
		msleep(mpro->request_delay);

	/* Vaihe 3: sub-info 0xB7 IN, len=5 */
	ret = usb_control_msg(mpro->udev, usb_rcvctrlpipe(mpro->udev, 0),
			      0xB7, 0xC0, 0, 0, buf, 5, MPRO_USB_TIMEOUT_MS);
	if (ret < 0)
		goto out;

	if (mpro->request_delay)
		msleep(mpro->request_delay);

	/* Vaihe 4: firmware-string 0xA0 IN, len=64 */
	ret = usb_control_msg(mpro->udev, usb_rcvctrlpipe(mpro->udev, 0),
			      0xA0, 0xC0, 0, 0, buf, 64, MPRO_USB_TIMEOUT_MS);
	if (ret > 0) {
		size_t copy = min((size_t)ret, outlen - 1);
		memcpy(out, buf, copy);
		out[copy] = '\0';
		out[strnlen(out, copy)] = '\0';
	}

out:
	mutex_unlock(&mpro->lock);
	return ret;
}

static int mpro_parse_firmware_version(const char *str, s16 *major, s16 *minor)
{
	unsigned int maj, min;
	const char *p = str;

	if (!str || !major || !minor)
		return -EINVAL;

	/* Hyppää alkuun "v" tai "V" jos se on olemassa */
	if (*p == 'v' || *p == 'V')
		p++;

	if (sscanf(p, "%u.%u", &maj, &min) != 2)
		return -EINVAL;

	if (maj > S16_MAX || min > S16_MAX)
		return -ERANGE;

	*major = (s16) maj;
	*minor = (s16) min;
	return 0;
}

int mpro_get_firmware(struct mpro_device *mpro)
{
	int ret;

	/* Oletukset epäonnistumisen varalle */
	mpro->fw_major = -1;
	mpro->fw_minor = -1;
	strscpy(mpro->fw_string, "unknown", sizeof(mpro->fw_string));

	ret = mpro_get_firmware_string(mpro, mpro->fw_string,
				       sizeof(mpro->fw_string));
	if (ret < 0)
		return ret;
	if (ret == 0)
		return -EIO;

	if (mpro_parse_firmware_version(mpro->fw_string,
					&mpro->fw_major, &mpro->fw_minor) < 0) {
		dev_warn(&mpro->intf->dev,
			 "could not parse firmware version from '%s'\n",
			 mpro->fw_string);
		/* String on silti tallennettu, mutta version-numerot jäävät -1 */
	}

	return 0;
}

EXPORT_SYMBOL_GPL(mpro_get_firmware);
