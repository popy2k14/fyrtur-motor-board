/*
 * motor.c
 *
 *  Created on: Aug 29, 2020
 *      Author: markojuhanne
 */
#include "main.h"
#include "motor.h"
#include "eeprom.h"

enum motor_status status;
enum motor_direction direction;

#define DEG_TO_LOCATION(x) (GEAR_RATIO * x / 360)



/* LOCATION is the spatial position of the curtain measured in motor revolutions. Due to additional gear mechanism, it takes
 * GEAR_RATIO revolutions in order to actually reach 1 full revolution of the curtain rod. Motor revolution is detected by HALL sensor,
 * which generates 1 interrupt (tick) per motor revolution.
 *
 * POSITION itself is a measure of curtain position reported by float between 0.0 (fully closed) and 100.0 (fully open) and can be
 * calculated from LOCATION with location_to_position100 (and vice versa with position100_to_location).
 *
 * Maximum POSITION is affected by user-customizable soft_lower_limit (configured via CMD_SET_SOFT_LIMIT). In addition to this, there is
 * the hard_lower_limit that mimics the "hard-coded / absolute" maximum open position of the original Fyrtur firmware. However this
 * "maximum" position can be ignored using CMD_OVERRIDE_XXX commands and also be re-configured with CMD_SET_HARD_LIMIT command.
 */
uint32_t target_location = 0;
int32_t location = 0;
uint32_t hard_lower_limit = DEFAULT_HARD_LOWER_LIMIT;
uint32_t soft_lower_limit;

uint16_t minimum_voltage;	// value is minimum voltage * 16 (float stored as interger value)

/* the motor driver gate PWM duty cycle is initially 80/255 when first energized and then adjusted according to target_speed */
#define INITIAL_PWM 80

uint8_t default_speed;
uint8_t target_speed = 0; // target RPM
uint8_t curr_pwm = 0;  // PWM setting

/*
 * When resetting we forget our position and wait until motor has stalled. When that happens, assume that we are in top
 * position and reset location to 0.
 */
uint8_t resetting = 0;

/*
 * count how many milliseconds since previous HALL sensor interrupt occurred
 * in order to calculate hall_interval (and RPM) and also to detect motor stalling
 */
uint32_t hall_idle_time = 0;

uint32_t hall_interval = 0; // how many milliseconds between hall sensor ticks (in order to calculate motor RPM)

uint32_t movement_started_timestamp = 0;	// used for stall detection grace period (let motor some time to energize before applying stall detecion)

enum motor_command command; // for deferring execution to main loop since we don't want to invoke HAL_Delay in UARTinterrupt handler

// ----- Commands supported also by original Fyrtur module -----

// commands with 1 parameter
#define CMD_GO_TO	0xdd

// commands without parameter
#define CMD_UP 		0x0add
#define CMD_DOWN 	0x0aee
#define CMD_UP_17 	0x0a0d
#define CMD_DOWN_17	0x0a0e
#define CMD_STOP	0x0acc

#define CMD_OVERRIDE_UP_90		0xfad1
#define CMD_OVERRIDE_DOWN_90	0xfad2
#define CMD_OVERRIDE_UP_6		0xfad3
#define CMD_OVERRIDE_DOWN_6		0xfad4
#define CMD_SET_SOFT_LIMIT		0xfaee	// will be stored to flash memory
#define CMD_SET_HARD_LIMIT		0xfacc	// will be stored to flash memory
#define CMD_RESET_SOFT_LIMIT	0xfa00	// will cause soft limit to be reseted to same value as hard limit and to be stored to flash memory

#define CMD_GET_STATUS 	0xcccc
#define CMD_GET_STATUS2 0xcccd
#define CMD_GET_STATUS3 0xccce
#define CMD_GET_STATUS4 0xccdd

// ------ Commands supported only by our custom firmware -------

// commands with 1 parameter
#define CMD_EXT_GO_TO				0x10	// target position is the lower 4 bits of the 1st byte + 2nd byte (12 bits of granularity), where lower 4 bits is the decimal part
#define CMD_EXT_SET_SPEED 			0x20	// setting speed via this command will not alter non-volatile memory (so it's safe for limited write-cycle flash memory)
#define CMD_EXT_SET_DEFAULT_SPEED 	0x30	// default speed will be stored to flash memory
#define CMD_EXT_SET_MINIMUM_VOLTAGE	0x40	// minimum voltage. Will be stored to flash memory

// commands without parameter
#define CMD_EXT_GET_VERSION 		0xccdc
#define CMD_EXT_GET_STATUS 			0xccde
#define CMD_EXT_GET_LIMITS 			0xccdf

/****************** EEPROM variables ********************/

typedef enum eeprom_var_t {
	SOFT_LOWER_LIMIT_EEPROM = 0,
	HARD_LOWER_LIMIT_EEPROM = 1,
	MINIMUM_VOLTAGE_EEPROM = 2,
	DEFAULT_SPEED_EEPROM = 3
} eeprom_var_t;

/* Virtual address defined by the user: 0xFFFF value is prohibited */
uint16_t VirtAddVarTab[NB_OF_VAR] = {0x5555, 0x6666, 0x7777, 0x8888};
//uint16_t VarDataTab[NB_OF_VAR] = {0, 0, 0};
//uint16_t VarValue = 0;


/*for (VarValue = 1; VarValue <= 0x1000; VarValue++)
{
  EE_WriteVariable(VirtAddVarTab[SOFT_LOWER_LIMIT_EEPROM], VarValue);
}

EE_ReadVariable(VirtAddVarTab[0], &VarDataTab[0]);
*/

void motor_set_default_settings() {
	soft_lower_limit = DEFAULT_HARD_LOWER_LIMIT; // by default, soft_lower_limit is hard_lower_limit
	hard_lower_limit = DEFAULT_HARD_LOWER_LIMIT;
	minimum_voltage = DEFAULT_MINIMUM_VOLTAGE;
	default_speed = DEFAULT_TARGET_SPEED;
}

void motor_load_settings() {
	uint16_t tmp;
	if (EE_ReadVariable(VirtAddVarTab[SOFT_LOWER_LIMIT_EEPROM], &tmp) != 0) {
		tmp = soft_lower_limit = DEFAULT_HARD_LOWER_LIMIT;	// by default, soft_lower_limit is hard_lower_limit
		EE_WriteVariable(VirtAddVarTab[SOFT_LOWER_LIMIT_EEPROM], tmp);
	} else {
		soft_lower_limit = tmp;
	}
	if (EE_ReadVariable(VirtAddVarTab[HARD_LOWER_LIMIT_EEPROM], &tmp) != 0) {
		tmp = hard_lower_limit = DEFAULT_HARD_LOWER_LIMIT;
		EE_WriteVariable(VirtAddVarTab[HARD_LOWER_LIMIT_EEPROM], tmp);
	} else {
		hard_lower_limit = tmp;
	}
	if (EE_ReadVariable(VirtAddVarTab[MINIMUM_VOLTAGE_EEPROM], &tmp) != 0) {
		minimum_voltage = DEFAULT_MINIMUM_VOLTAGE;
		tmp = minimum_voltage;
		EE_WriteVariable(VirtAddVarTab[MINIMUM_VOLTAGE_EEPROM], tmp);
	} else {
		minimum_voltage = tmp;
	}
	if (EE_ReadVariable(VirtAddVarTab[DEFAULT_SPEED_EEPROM], &tmp) != 0) {
		tmp = default_speed = DEFAULT_TARGET_SPEED;
		EE_WriteVariable(VirtAddVarTab[DEFAULT_SPEED_EEPROM], tmp);
	} else {
		default_speed = tmp;
	}
}

void motor_write_setting( eeprom_var_t var, uint16_t value ) {
	uint16_t tmp;
	if (status == Stopped) {
		// motor has to be stopped to change non-volatile settings (writing to FLASH should occur uninterrupted)
		EE_ReadVariable(VirtAddVarTab[var], &tmp);
		if (tmp != value) {
			EE_WriteVariable(VirtAddVarTab[var], value);
		}
	}
}


uint32_t position100_to_location( float position ) {
	if (position > 100)
		return 100;
	return position*soft_lower_limit/100;
}


float location_to_position100() {
	if (resetting) {
		// When resetting we forget our position and return 50% instead
		return 50;
	}
	if (location < 0) {
		return 0;
	}
	if (location > soft_lower_limit) {
		return 100;
	}
	return 100*location/soft_lower_limit;
}


float get_rpm() {
	if (hall_interval)
		return 60*1000/GEAR_RATIO/hall_interval;
	return 0;
}


void process_location() {
	if (direction == Up) {
		location--;
		if (location <= target_location) {
			motor_stop();
		}
	} else if (direction == Down) {
		location++;

		if(location >= target_location) {
			motor_stop();
		}
	}
}

void hall_sensor_callback( uint8_t sensor ) {
	if (sensor == HALL_1_SENSOR) {
		hall_interval = hall_idle_time;	// update time passed between hall sensor interrupts
		hall_idle_time = 0;
		if (!resetting)
			process_location();
	} else if (sensor == HALL_2_SENSOR) {
		// We actually use only HALL #1 sensor for RPM calculation and stall detection
	}
}


void motor_adjust_rpm() {
	uint32_t speed = get_rpm();
	if (speed < target_speed) {
		if (curr_pwm < 255) {
			curr_pwm ++;
			if (direction == Up)
				TIM1->CCR4 = curr_pwm;
			else
				TIM1->CCR1 = curr_pwm;
		}
	}

	if (speed > target_speed) {
		if (curr_pwm > 0) {
			curr_pwm --;
			if (direction == Up)
				TIM1->CCR4 = curr_pwm;
			else
				TIM1->CCR1 = curr_pwm;
		}
	}
}


/*
 * This is periodically (every 1 millisecond) called SysTick_Handler
 */
void motor_stall_check() {
	if (status == Moving) {
		// Count how many milliseconds since previous HALL sensor interrupt
		// in order to calculate RPM and detect motor stalling
		hall_idle_time ++;
		if (HAL_GetTick() - movement_started_timestamp > MOVEMENT_GRACE_PERIOD) {
			// enough time has passed since motor is energized -> apply stall detection

			if (hall_idle_time > HALL_TIMEOUT) {
				// motor has stalled/stopped
				motor_stopped();
				hall_idle_time = 0;
			}
		}
	}
}

void motor_stopped() {
	if (status != Stopped) {
		// motor has stalled!

		hall_interval = 0;
		if (resetting) {
			// we reached top position
			resetting = 0;
		}

		// If motor has stalled, we assume that we have reached the top position.
		location = 0;

		// De-energize the motor just in case..
		motor_stop();
		hall_idle_time = 0;
	}
}


void motor_stop() {

	// Make sure that all mosfets are off
	pwm_stop(LOW1_PWM_CHANNEL);
	pwm_stop(LOW2_PWM_CHANNEL);
	// Remember also to set GPIO_PULLDOWN in HAL_TIM_MspPostInit ! (generated automatically by CubeMX)

	HAL_GPIO_WritePin(HIGH_1_GATE_GPIO_Port, HIGH_1_GATE_Pin, GPIO_PIN_RESET);
	HAL_GPIO_WritePin(HIGH_2_GATE_GPIO_Port, HIGH_2_GATE_Pin, GPIO_PIN_RESET);
	TIM1->CCR1 = 0;
	TIM1->CCR4 = 0;
	status = Stopped;
	direction = None;
	curr_pwm = 0;
	hall_interval = 0;
	target_speed = 0;
}



void motor_up(uint8_t motor_speed) {
	// reset stall detection timeout
	hall_idle_time = 0;
	movement_started_timestamp = HAL_GetTick();
	motor_stop();
	HAL_Delay(10);

	// turn on LOW2 PWM and HIGH1
	pwm_start(LOW2_PWM_CHANNEL);
	target_speed = motor_speed;
	TIM1->CCR4 = INITIAL_PWM;
	curr_pwm = INITIAL_PWM;
	HAL_GPIO_WritePin(HIGH_1_GATE_GPIO_Port, HIGH_1_GATE_Pin, GPIO_PIN_SET);
	direction = Up;
	status = Moving;
}

void motor_down(uint8_t motor_speed) {

	// reset stall detection timeout
	hall_idle_time = 0;
	movement_started_timestamp = HAL_GetTick();
	motor_stop();
	HAL_Delay(10);

	// turn on LOW1 PWM and HIGH2
	pwm_start(LOW1_PWM_CHANNEL);
	target_speed = motor_speed;
	TIM1->CCR1 = INITIAL_PWM;
	curr_pwm = INITIAL_PWM;
	HAL_GPIO_WritePin(HIGH_2_GATE_GPIO_Port, HIGH_2_GATE_Pin, GPIO_PIN_SET);
	direction = Down;
	status = Moving;
}

uint8_t check_voltage() {
	if (minimum_voltage != 0) {
		uint16_t voltage = get_voltage() / 30;
		if (voltage < minimum_voltage)
			return 0;
	}
	return 1;
}

void motor_process() {
	if (command == MotorUp) {
		motor_up(default_speed);
		command = NoCommand;
	} else if (command == MotorDown) {
		motor_down(default_speed);
		command = NoCommand;
	} else if(command == Stop) {
		motor_stop();
		command = NoCommand;
	}
}


uint8_t calculate_battery() {
	return 0x12; // TODO
}



uint8_t handle_command(uint8_t * rx_buffer, uint8_t * tx_buffer, uint8_t burstindex, uint8_t * tx_bytes) {
	uint8_t cmd1, cmd2;
	cmd1 = rx_buffer[3];
	cmd2 = rx_buffer[4];
	uint16_t cmd = (cmd1 << 8) + cmd2;

	uint8_t cmd_handled = 1;

	switch (cmd) {

		case CMD_GET_STATUS:
			{
				tx_buffer[0] = 0x00;
				tx_buffer[1] = 0xff;
				tx_buffer[2] = 0xd8;
				tx_buffer[3] = calculate_battery();
				tx_buffer[4] = (uint8_t)( get_voltage()/16);  // returned value is voltage*30
				tx_buffer[5] = (uint8_t)get_rpm();
				tx_buffer[6] = location_to_position100();
				tx_buffer[7] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6];
				*tx_bytes=8;
			}
			break;

		case CMD_UP:
			{
				target_location = 0;
				command = MotorUp;
			}
			break;

		case CMD_DOWN:
			{
				target_location = soft_lower_limit;
				command = MotorDown;
			}
			break;

		case CMD_UP_17:
			{
				target_location -= DEG_TO_LOCATION(17);
				if (target_location < 0)
					target_location = 0;
				command = MotorUp;
			}
			break;

		case CMD_DOWN_17:
			{
				target_location += DEG_TO_LOCATION(17);
				if (target_location > soft_lower_limit)
					target_location = soft_lower_limit;
				command = MotorDown;
			}
			break;

		case CMD_STOP:
			{
				command = Stop;
			}
			break;

		case CMD_OVERRIDE_UP_90:
			{
				target_location -= DEG_TO_LOCATION(90);
				command = MotorUp;
			}
			break;

		case CMD_OVERRIDE_DOWN_90:
			{
				target_location += DEG_TO_LOCATION(90);
				command = MotorDown;
			}
			break;

		case CMD_OVERRIDE_UP_6:
			{
				target_location -= DEG_TO_LOCATION(6);
				command = MotorUp;
			}
			break;

		case CMD_OVERRIDE_DOWN_6:
			{
				target_location += DEG_TO_LOCATION(6);
				command = MotorDown;
			}
			break;

		case CMD_SET_SOFT_LIMIT:
			{
				motor_write_setting(SOFT_LOWER_LIMIT_EEPROM, location);
				soft_lower_limit = location;
			}
			break;

		case CMD_SET_HARD_LIMIT:
			{
				motor_write_setting(HARD_LOWER_LIMIT_EEPROM, location);
				hard_lower_limit = location;
			}
			break;

		case CMD_RESET_SOFT_LIMIT:
			{
				motor_write_setting(SOFT_LOWER_LIMIT_EEPROM, hard_lower_limit);
				soft_lower_limit = hard_lower_limit;
				resetting = 1;
			}
			break;

		case CMD_EXT_GET_VERSION:
			{
				tx_buffer[0] = 0x00;
				tx_buffer[1] = 0xff;
				tx_buffer[2] = 0xd0;
				tx_buffer[3] = VERSION_MAJOR;
				tx_buffer[4] = VERSION_MINOR;
				tx_buffer[5] = minimum_voltage;
				tx_buffer[6] = 0;
				tx_buffer[7] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6];
				*tx_bytes=8;
			}
			break;

		case CMD_EXT_GET_STATUS:
			{
				tx_buffer[0] = 0x00;
				tx_buffer[1] = 0xff;
				tx_buffer[2] = 0xda;
				tx_buffer[3] = status;
				tx_buffer[4] = (uint8_t)(get_motor_current());
				uint16_t pos = location_to_position100() * 256;
				tx_buffer[5] = pos >> 8;
				tx_buffer[6] = pos & 0xff;
				tx_buffer[7] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6];
				*tx_bytes=8;
			}
			break;

		case CMD_EXT_GET_LIMITS:
			{
				tx_buffer[0] = 0x00;
				tx_buffer[1] = 0xff;
				tx_buffer[2] = 0xdb;
				tx_buffer[3] = resetting;
				tx_buffer[4] = soft_lower_limit >> 8;
				tx_buffer[5] = soft_lower_limit & 0xff;
				tx_buffer[6] = hard_lower_limit >> 8;
				tx_buffer[7] = hard_lower_limit & 0xff;
				tx_buffer[8] = tx_buffer[3] ^ tx_buffer[4] ^ tx_buffer[5] ^ tx_buffer[6] ^ tx_buffer[7];
				*tx_bytes=9;
			}
			break;

		default:
			cmd_handled=0;
	}

	if (!cmd_handled) {
		// one byte commands with parameter
		if (cmd1 == CMD_EXT_SET_SPEED) {
			if (cmd2 > 1) {
				default_speed = cmd2;
				if (target_speed != 0)
					target_speed = cmd2;
			}
		} else if (cmd1 == CMD_EXT_SET_DEFAULT_SPEED) {
			if (cmd2 > 0) {
				motor_write_setting(DEFAULT_SPEED_EEPROM, cmd2);
				default_speed = cmd2;
			}
		} else if (cmd1 == CMD_GO_TO) {
			if (!resetting) {
				target_location = position100_to_location(cmd2);
				if (target_location < location) {
					command = MotorUp;
				} else {
					command = MotorDown;
				}
			}
		} else if ((cmd1 & 0xf0) == CMD_EXT_GO_TO) {
			if (!resetting) {
				uint16_t pos = ((cmd1 & 0x0f)<<8) + cmd2;
				float pos2 = ((float)pos)/16;
				target_location = position100_to_location(pos2);
				if (target_location < location) {
					command = MotorUp;
				} else {
					command = MotorDown;
				}
			}
		} else if (cmd1 == CMD_EXT_SET_MINIMUM_VOLTAGE) {
			motor_write_setting(MINIMUM_VOLTAGE_EEPROM, cmd2);
			minimum_voltage = cmd2;
		}
	}

	return 1;
}

void motor_init() {
	resetting = 0;
	direction = None;
	command = NoCommand;
	motor_stop();

	location = soft_lower_limit; // assume we are at bottom position

#ifdef AUTO_RESET
	resetting = 1;
	motor_up();
#endif
}




