#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "board.h"
#include "can.h"
#include "can_common.h"
#include "config.h"
#include "device.h"
#include "dfu.h"
#include "gpio.h"
#include "gs_usb.h"
#include "hal_include.h"
#include "led.h"
#include "timer.h"
#include "usbd_conf.h"
#include "usbd_core.h"
#include "usbd_def.h"
#include "usbd_desc.h"
#include "usbd_gs_can.h"
#include "util.h"

static USBD_GS_CAN_HandleTypeDef hGS_CAN;
static USBD_HandleTypeDef hUSB = {0};

/* 心跳 LED 状态变量 - 必须在函数外部或函数开头定义 */
#if defined(BOARD_Woloong_U2C)
static uint32_t heartbeat_last_tick = 0;
static uint8_t heartbeat_state = 0;
static uint8_t heartbeat_selftest_done = 0;
#endif

void __weak _close(void) {
}
void __weak _lseek(void) {
}
void __weak _read(void) {
}
void __weak _write(void) {
}

int main(void)
{
	HAL_Init();
	device_sysclock_config();

	gpio_init();
	timer_init();

	INIT_LIST_HEAD(&hGS_CAN.list_frame_pool);
	INIT_LIST_HEAD(&hGS_CAN.list_to_host);

	for (unsigned i = 0; i < ARRAY_SIZE(hGS_CAN.msgbuf); i++) {
		list_add_tail(&hGS_CAN.msgbuf[i].list, &hGS_CAN.list_frame_pool);
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(hGS_CAN.channels); i++) {
		const struct board_channel_config *channel_config = &config.channel[i];
		can_data_t *channel = &hGS_CAN.channels[i];

		can_channel_set_nr(channel, i);

		INIT_LIST_HEAD(&channel->list_from_host);

		led_init(&channel->leds,
				 LEDRX_GPIO_Port, LEDRX_Pin, LEDRX_Active_High,
				 LEDTX_GPIO_Port, LEDTX_Pin, LEDTX_Active_High);

		can_init(channel, channel_config);
		can_disable(channel);
	}

	USBD_Init(&hUSB, (USBD_DescriptorsTypeDef*)&FS_Desc, DEVICE_FS);
	USBD_RegisterClass(&hUSB, &USBD_GS_CAN);
	USBD_GS_CAN_Init(&hGS_CAN, &hUSB);
	USBD_Start(&hUSB);

	/* nice wake-up pattern - 原有 RX/TX LED 交替闪烁 */
	for (uint8_t j = 0; j < 10; j++) {
		HAL_GPIO_TogglePin(LEDRX_GPIO_Port, LEDRX_Pin);
		HAL_Delay(50);
		HAL_GPIO_TogglePin(LEDTX_GPIO_Port, LEDTX_Pin);
		HAL_Delay(50);
	}

	/* ========== 上电自检：PC13 快速闪烁 3 次 ========== */
#if defined(BOARD_Woloong_U2C)
	for (int i = 0; i < 3; i++) {
		/* LED 点亮（低电平） */
		HAL_GPIO_WritePin(LEDRUN_GPIO_Port, LEDRUN_Pin, GPIO_PIN_RESET);
		HAL_Delay(100);
		/* LED 熄灭（高电平） */
		HAL_GPIO_WritePin(LEDRUN_GPIO_Port, LEDRUN_Pin, GPIO_PIN_SET);
		HAL_Delay(100);
	}
	/* 自检完成后熄灭，初始化心跳状态 */
	HAL_GPIO_WritePin(LEDRUN_GPIO_Port, LEDRUN_Pin, GPIO_PIN_SET);
	heartbeat_selftest_done = 1;
	heartbeat_last_tick = HAL_GetTick();
#endif

	while (1) {
		for (unsigned int i = 0; i < ARRAY_SIZE(hGS_CAN.channels); i++) {
			can_data_t *channel = &hGS_CAN.channels[i];
			CAN_SendFrame(&hGS_CAN, channel);
		}

		USBD_GS_CAN_ReceiveFromHost(&hUSB);
		USBD_GS_CAN_SendToHost(&hUSB);

		for (unsigned int i = 0; i < ARRAY_SIZE(hGS_CAN.channels); i++) {
			can_data_t *channel = &hGS_CAN.channels[i];
			CAN_ReceiveFrame(&hGS_CAN, channel);
			CAN_HandleError(&hGS_CAN, channel);
			led_update(&channel->leds);
		}

		if (USBD_GS_CAN_DfuDetachRequested(&hUSB)) {
			dfu_run_bootloader();
		}

#if defined(BOARD_Woloong_U2C)
		/* 心跳 LED：1.5s 间隔闪烁 */
		if (heartbeat_selftest_done) {
			uint32_t now = HAL_GetTick();
			if ((now - heartbeat_last_tick) >= 1500) {
				heartbeat_last_tick = now;
				heartbeat_state = !heartbeat_state;
				/* 低电平有效：state=1 点亮，state=0 熄灭 */
				HAL_GPIO_WritePin(LEDRUN_GPIO_Port, LEDRUN_Pin,
					heartbeat_state ? GPIO_PIN_RESET : GPIO_PIN_SET);
			}
		}
#endif
	}  /* while(1) 结束 */

	return 0;  /* 永远不会执行到这里，但保持规范 */
}
