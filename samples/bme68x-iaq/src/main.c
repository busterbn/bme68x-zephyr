/*
 * Copyright (c) 2024, Chris Duf
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Index for Air Quality (IAQ) with BSEC and the BME68X Sensor API.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/bme68x_sensor_api2.h>		// Chriss duff

#include "bme68x.h"							// Bosch

#include "bme68x_iaq2.h"						// Chriss duff (Samme navn som bosch's)

LOG_MODULE_REGISTER(app, CONFIG_BME68X_SAMPLE_LOG_LEVEL);

/*
 * We'll print only integer values to not impose additional configuration
 * for supporting floats in format string specifiers.
 */
struct fixed_point {
	/* Integer part. */
	int32_t q;
	/* Fractional digits (variable precision). */
	uint32_t r;
};
struct iaq_output {
	/*
	 * Temperature measured by BME680/688 in degree Celsius.
	 * Precision 1/100 degC (2-digits remainder).
	 */
	struct fixed_point raw_temperature;
	/*
	 * Pressure measured by the BME680/688 in kPa.
	 * Precision 1 Pa (3-digits remainder).
	 */
	struct fixed_point raw_pressure;
	/*
	 * Relative directly measured by the BME680/688 in %.
	 * Precision 1/100 percent (2-digits remainder).
	 */
	struct fixed_point raw_humidity;
	/*
	 * Gas resistance measured by the BME680/688 in kOhm.
	 * Precision 1 Ohm (3-digits remainder).
	 */
	struct fixed_point raw_gas_res;
	/*
	 * Sensor heat compensated temperature in degrees Celsius.
	 * Precision 1/100 degC (2-digits remainder).
	 */
	struct fixed_point temperature;
	/*
	 * Sensor heat compensated relative humidity in %.
	 * Precision 1/100 percent (2-digits remainder).
	 */
	struct fixed_point humidity;
	/* Scaled IAQ [0,500]. */
	uint16_t iaq;
	enum bme68x_iaq_accuracy iaq_accuracy;
	/* Unscaled IAQ, range unknown. */
	uint32_t static_iaq;
	/* CO2 equivalent estimate in ppm. */
	uint32_t co2_equivalent;
	enum bme68x_iaq_accuracy co2_accuracy;
	/*
	 * VOC estimate in ppm.
	 * Precision 1/100 ppm (2-digits remainder).
	 */
	struct fixed_point voc_equivalent;
	enum bme68x_iaq_accuracy voc_accuracy;
	enum bme68x_iaq_status stab_status;
	enum bme68x_iaq_status run_status;
};

/* Log IAQ samples. */										// bme68x_iaq.c	(Chriss duff)
static void iaq_output_handler(struct bme68x_iaq_sample const *iaq_sample);

int main(void)
{
	/* Any compatible device will be fine. */
	struct device const *const dev = DEVICE_DT_GET_ONE(bosch_bme68x_sensor_api);

	struct bme68x_dev bme68x_dev = {0};
	int ret = bme68x_sensor_api_init(dev, &bme68x_dev);		// bme68x_drv.c	(Chriss duff)
	if (!ret) {
		ret = bme68x_init(&bme68x_dev);						// bme68c.h
	}
	if (ret) {
		LOG_ERR("sensor initialization failed: %d", ret);
		goto sleep_forever;
	}

	ret = bme68x_iaq_init();								// bme68x_iaq.c	(Chriss duff)
	if (ret) {
		LOG_ERR("IAQ initialization failed: %d", ret);
		goto sleep_forever;
	}

	/* Enter BSEC control loop. */
	bme68x_iaq_run(&bme68x_dev, iaq_output_handler);		// bme68x_iaq.c	(Chriss duff)

sleep_forever:
	k_sleep(K_FOREVER);
	return 0;
}

static inline void fixed_point_init(float const x, ssize_t const precision,
				    struct fixed_point *fixed_point)
{
	int32_t x_scaled = x * precision;
	fixed_point->q = x_scaled / precision;
	if (fixed_point->q < 0) {
		fixed_point->r = (fixed_point->q * precision) - x_scaled;
	} else {
		fixed_point->r = x_scaled - (fixed_point->q * precision);
	}
}

static void iaq_output_init(struct bme68x_iaq_sample const *iaq_sample,
			    struct iaq_output *iaq_output)
{
	/* degC to degC, centidegrees precision. */
	fixed_point_init(iaq_sample->raw_temperature, 100, &iaq_output->raw_temperature);
	fixed_point_init(iaq_sample->temperature, 100, &iaq_output->temperature);
	/* Pa to kPa, Pa precision */
	fixed_point_init(iaq_sample->raw_pressure / 1000.0f, 1000, &iaq_output->raw_pressure);
	/* percent to percent, centipercent precision. */
	fixed_point_init(iaq_sample->raw_humidity, 100, &iaq_output->raw_humidity);
	fixed_point_init(iaq_sample->humidity, 100, &iaq_output->humidity);
	/* Ohm to kOhm, Ohm precision. */
	fixed_point_init(iaq_sample->raw_gas_res / 1000.0f, 1000, &iaq_output->raw_gas_res);
	/* IAQ scaled to [0,500]. */
	iaq_output->iaq = (uint16_t)iaq_sample->iaq;
	iaq_output->iaq_accuracy = iaq_sample->iaq_accuracy;
	/* Unscaled IAQ, range unknown. */
	iaq_output->static_iaq = (uint32_t)iaq_sample->static_iaq;
	/* ppm. */
	iaq_output->co2_equivalent = (uint32_t)iaq_sample->co2_equivalent;
	iaq_output->co2_accuracy = iaq_sample->co2_accuracy;
	/* ppm, 1/100 ppm precision. */
	fixed_point_init(iaq_sample->voc_equivalent, 100, &iaq_output->voc_equivalent);
	iaq_output->voc_accuracy = iaq_sample->voc_accuracy;
	iaq_output->stab_status = iaq_sample->stab_status;
	iaq_output->run_status = iaq_sample->run_status;
}

void iaq_output_handler(struct bme68x_iaq_sample const *iaq_sample)
{
	static char const *accuracy2str[] = {
		[MY_BME68X_IAQ_ACCURACY_UNRELIABLE] = "unreliable",
		[MY_BME68X_IAQ_ACCURACY_LOW] = "low accuracy",
		[MY_BME68X_IAQ_ACCURACY_MEDIUM] = "medium accuracy",
		[MY_BME68X_IAQ_ACCURACY_HIGH] = "high accuracy",
	};
	static char const *stab2str[] = {
		[MY_BME68X_IAQ_STAB_ONGOING] = "on-going",
		[MY_BME68X_IAQ_STAB_FINISHED] = "finished",
	};

	struct iaq_output iaq_output;
	iaq_output_init(iaq_sample, &iaq_output);

	LOG_INF("-- IAQ output signals (%d) --", iaq_sample->cnt_outputs);
	LOG_INF("T:%d.%02u degC", iaq_output.raw_temperature.q, iaq_output.raw_temperature.r);
	LOG_INF("P:%d.%03u kPa", iaq_output.raw_pressure.q, iaq_output.raw_pressure.r);
	LOG_INF("H:%d.%02u %%", iaq_output.raw_humidity.q, iaq_output.raw_humidity.r);
	LOG_INF("G:%d.%03u kOhm", iaq_output.raw_gas_res.q, iaq_output.raw_gas_res.r);
	LOG_INF("IAQ:%u (%s)", iaq_output.iaq, accuracy2str[iaq_output.iaq_accuracy]);
	LOG_INF("CO2:%u ppm (%s)", iaq_output.co2_equivalent,
		accuracy2str[iaq_output.co2_accuracy]);
	LOG_INF("VOC:%d.%02u ppm (%s)", iaq_output.voc_equivalent.q, iaq_output.voc_equivalent.r,
		accuracy2str[iaq_output.voc_accuracy]);
	LOG_INF("stabilization: %s, %s", stab2str[iaq_output.stab_status],
		stab2str[iaq_output.run_status]);
}
