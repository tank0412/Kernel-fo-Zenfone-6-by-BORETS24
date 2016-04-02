/*
 * STMicroelectronics lsm6ds3 trigger driver
 *
 * Copyright 2014 STMicroelectronics Inc.
 *
 * Denis Ciocca <denis.ciocca@st.com>
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/interrupt.h>
#include <linux/iio/events.h>

#include "st_lsm6ds3.h"

#define ST_LSM6DS3_SRC_FUNC_ADDR		0x53
#define ST_LSM6DS3_FIFO_DATA_AVL_ADDR		0x3b

#define ST_LSM6DS3_SRC_STEP_DETECTOR_DATA_AVL	0x10
#define ST_LSM6DS3_SRC_TILT_DATA_AVL		0x20
#define ST_LSM6DS3_SRC_STEP_COUNTER_DATA_AVL	0x80
#define ST_LSM6DS3_FIFO_DATA_AVL		0x80
#define ST_LSM6DS3_FIFO_DATA_OVR		0x40

static struct workqueue_struct *st_lsm6ds3_wq;

void st_lsm6ds3_flush_works()
{
	pr_info("st_lsm6ds3_flush_works");
	flush_workqueue(st_lsm6ds3_wq);
}

irqreturn_t st_lsm6ds3_save_timestamp(int irq, void *private)
{
	struct timespec ts;
	struct lsm6ds3_data *cdata = private;

	get_monotonic_boottime(&ts);
	cdata->timestamp = timespec_to_ns(&ts);
	queue_work(st_lsm6ds3_wq, &cdata->data_work);

	disable_irq_nosync(irq);

	return IRQ_HANDLED;
}

static void st_lsm6ds3_irq_management(struct work_struct *data_work)
{
	struct lsm6ds3_data *cdata;
	u8 src_value = 0x00, src_fifo = 0x00;

	cdata = container_of((struct work_struct *)data_work,
						struct lsm6ds3_data, data_work);

	cdata->tf->read(cdata, ST_LSM6DS3_SRC_FUNC_ADDR, 1, &src_value, true);
	cdata->tf->read(cdata, ST_LSM6DS3_FIFO_DATA_AVL_ADDR, 1,
							&src_fifo, true);

	dev_dbg(cdata->dev, "st_lsm6ds3_irq_management src_value=%x, src_fifo=%x\n", src_value, src_fifo);
	if (src_fifo & ST_LSM6DS3_FIFO_DATA_AVL) {
		dev_dbg(cdata->dev, "ST_LSM6DS3_FIFO_DATA_AVL\n");
		if (src_fifo & ST_LSM6DS3_FIFO_DATA_OVR) {
			dev_err(cdata->dev,
				"data fifo overrun, reduce fifo size.\n");
		}
		mutex_lock(&cdata->fifo_lock);
		st_lsm6ds3_read_fifo(cdata, false);
		mutex_unlock(&cdata->fifo_lock);

	}

	if (src_value & ST_LSM6DS3_SRC_STEP_DETECTOR_DATA_AVL) {
		dev_dbg(cdata->dev, "ST_LSM6DS3_SRC_STEP_DETECTOR_DATA_AVL\n");
		iio_push_event(cdata->indio_dev[ST_INDIO_DEV_STEP_DETECTOR],
				IIO_UNMOD_EVENT_CODE(IIO_STEP_DETECTOR,
				0, IIO_EV_TYPE_THRESH, IIO_EV_DIR_EITHER),
				cdata->timestamp);

		if (cdata->sign_motion_event_ready) {
			dev_dbg(cdata->dev, "sign_motion_event_ready\n");
			iio_push_event(cdata->indio_dev[
				ST_INDIO_DEV_SIGN_MOTION],
				IIO_UNMOD_EVENT_CODE(IIO_SIGN_MOTION,
				0, IIO_EV_TYPE_THRESH, IIO_EV_DIR_EITHER),
				cdata->timestamp);

			cdata->sign_motion_event_ready = false;
		}
	}

	if (src_value & ST_LSM6DS3_SRC_STEP_COUNTER_DATA_AVL) {
		dev_dbg(cdata->dev, "ST_LSM6DS3_SRC_STEP_COUNTER_DATA_AVL\n");
		iio_trigger_poll_chained(
				cdata->trig[ST_INDIO_DEV_STEP_COUNTER], 0);
	}

	if (src_value & ST_LSM6DS3_SRC_TILT_DATA_AVL) {
		dev_dbg(cdata->dev, "ST_LSM6DS3_SRC_TILT_DATA_AVL\n");
		iio_push_event(cdata->indio_dev[ST_INDIO_DEV_TILT],
				IIO_UNMOD_EVENT_CODE(IIO_TILT,
				0, IIO_EV_TYPE_THRESH, IIO_EV_DIR_EITHER),
				cdata->timestamp);
	}

	enable_irq(cdata->irq);
	return;
}

int st_lsm6ds3_allocate_triggers(struct lsm6ds3_data *cdata,
				const struct iio_trigger_ops *trigger_ops)
{
	int err, i, n;

	if (!st_lsm6ds3_wq)
		st_lsm6ds3_wq = create_workqueue(cdata->name);

	if (!st_lsm6ds3_wq)
		return -EINVAL;

	INIT_WORK(&cdata->data_work, st_lsm6ds3_irq_management);

	for (i = 0; i < ST_INDIO_DEV_NUM; i++) {
		cdata->trig[i] = iio_trigger_alloc("%s-trigger",
				cdata->indio_dev[i]->name);
		if (!cdata->trig[i]) {
			dev_err(cdata->dev,
					"failed to allocate iio trigger.\n");
			err = -ENOMEM;
			goto deallocate_trigger;
		}
		iio_trigger_set_drvdata(cdata->trig[i], cdata->indio_dev[i]);
		cdata->trig[i]->ops = trigger_ops;
		cdata->trig[i]->dev.parent = cdata->dev;
	}

	err = request_threaded_irq(cdata->irq, st_lsm6ds3_save_timestamp, NULL,
					IRQF_TRIGGER_HIGH, cdata->name, cdata);
	if (err)
		goto deallocate_trigger;

	for (n = 0; n < ST_INDIO_DEV_NUM; n++) {
		err = iio_trigger_register(cdata->trig[n]);
		if (err < 0) {
			dev_err(cdata->dev,
					"failed to register iio trigger.\n");
			goto free_irq;
		}
		cdata->indio_dev[n]->trig = cdata->trig[n];
	}

	return 0;

free_irq:
	free_irq(cdata->irq, cdata);
	for (n--; n >= 0; n--)
		iio_trigger_unregister(cdata->trig[n]);
deallocate_trigger:
	for (i--; i >= 0; i--)
		iio_trigger_free(cdata->trig[i]);

	return err;
}
EXPORT_SYMBOL(st_lsm6ds3_allocate_triggers);

void st_lsm6ds3_deallocate_triggers(struct lsm6ds3_data *cdata)
{
	int i;

	free_irq(cdata->irq, cdata);

	for (i = 0; i < ST_INDIO_DEV_NUM; i++)
		iio_trigger_unregister(cdata->trig[i]);
}
EXPORT_SYMBOL(st_lsm6ds3_deallocate_triggers);

MODULE_AUTHOR("Denis Ciocca <denis.ciocca@st.com>");
MODULE_DESCRIPTION("STMicroelectronics lsm6ds3 trigger driver");
MODULE_LICENSE("GPL v2");
