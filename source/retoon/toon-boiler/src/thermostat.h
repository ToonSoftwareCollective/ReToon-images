/* SPDX-License-Identifier: GPL-2.0-or-later */
/* toon-boilerd: the thermostat's wishes (thermostat.c) */
#ifndef THERMOSTAT_H
#define THERMOSTAT_H

#include <stdio.h>

#define TH_MIN		6.0
#define TH_MAX		30.0
#define N_PRESETS	4
#define HW_MIN		30.0	/* hot water; the boiler's own limits apply too */
#define HW_MAX		80.0

struct th {
	int hvac;		/* the thermostat is on */
	int active;		/* heat may be asked for: on, or frost protection */
	double target;		/* what the user (or the program) asked for */
	double eff_target;	/* what is worked towards now: pre-heating, frost */
	int preset;		/* 0..3, -1 = a temperature set by hand */
	int program, preheating, frost_active;
	char next[24];		/* "22:30 sleep": the program's next step */
	char until[8];		/* "22:30": a choice by hand holds until then */
	double out;		/* the flow temperature the rule asks for */
	int want;		/* ... which is above the heating threshold */
	int by_flow;		/* that decides (OpenTherm), not the room band */
	int pulse;		/* on/off contact: the rule decides in pulses */
	int pulse_on;		/* ... and this minute the contact is closed */
	double factor, loss, offset;	/* learned about the house */
	int cool_wanted;	/* cooling is switched on in the settings */
	int cool_ok;		/* ... and this boiler can do it */
	int season;		/* 1 = the cooling side of the thermostat */
	int cooling;		/* ... and working on it (frost protection goes first) */
	int auto_ok;		/* there is an outside temperature to go on */
	int external;		/* no boiler module: heat and cool asked for anyway,
				   for an external heater or airco (Home Assistant) */
	int can_heat;		/* heat can come now: the boiler module answers, or
				   an external heater is asked (set before th_tick) */
	int lag_stop;		/* stop the heat now: what is in the radiators
				   will bring the room to the target */
	int lag_nostart;	/* do not start: even a short burst overshoots */
	double flow_meas;	/* the boiler's own flow reading (OpenTherm id 25), -1 none */
	double ret_meas;	/* ... its return water (id 28), -1 none */
	double modulation;	/* ... its modulation in % (id 17), -1 none */
	int flame;		/* ... flame on (id 0), -1 not known */
};
extern struct th th;

void th_init(void);
int th_command(const char *line);	/* 1 = it was a thermostat command */
void th_source(const char *src);	/* who sends the next commands (for the change log) */
void th_sensor_changed(void);		/* the room temperature comes from elsewhere now */
/* can_cool: 1 yes, 0 no, -1 not known yet; outside only when out_ok */
void th_tick(int room_ok, double room, int heating, int opentherm, double max,
	     int can_cool, int out_ok, double outside);
double th_flow(double room, double max);
double th_hot_water(void);	/* degrees wanted now, 0 = the boiler's own */
int th_hot_water_keep_warm(void);	/* the hot water switch in OpenTherm id 0 */
void th_state(FILE *f);

#endif
