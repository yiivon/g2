/*
 * plan_line.c - acceleration managed line planning and motion execution
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2015 Alden S. Hart, Jr.
 * Copyright (c) 2012 - 2015 Rob Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "tinyg2.h"
#include "config.h"
#include "controller.h"
#include "canonical_machine.h"
#include "planner.h"
#include "stepper.h"
#include "report.h"
#include "util.h"
#include "spindle.h"

#include "xio.h"

// Using motate pins for profiling (see main.cpp)
// see https://github.com/synthetos/g2/wiki/Using-Pin-Changes-for-Timing-(and-light-debugging)
using namespace Motate;
extern OutputPin<kDebug1_PinNumber> debug_pin1;
extern OutputPin<kDebug2_PinNumber> debug_pin2;
extern OutputPin<kDebug3_PinNumber> debug_pin3;

// planner helper functions
static mpBuf_t *_plan_block(mpBuf_t *bf);
static void _calculate_override(mpBuf_t *bf);
static void _calculate_throttle(mpBuf_t *bf);
static void _calculate_jerk(mpBuf_t *bf);
static void _calculate_vmaxes(mpBuf_t *bf, const float axis_length[], const float axis_square[]);
static void _calculate_junction_vmax(mpBuf_t *bf);

//+++++DIAGNOSTICS
#pragma GCC optimize ("O0")
static void _set_diagnostics(mpBuf_t *bf)
{
    bf->linenum = bf->gm.linenum;
    bf->move_time_ms = bf->move_time * 60000;
    mb.move_time_ms = bf->move_time * 60000;
//    mb.arrival_rate_avg_ms = mb.arrival_rate_avg * 60000;
}
#pragma GCC reset_options


/* Runtime-specific setters and getters
 *
 * mp_zero_segment_velocity()         - correct velocity in last segment for reporting purposes
 * mp_get_runtime_velocity()          - returns current velocity (aggregate)
 * mp_get_runtime_machine_position()  - returns current axis position in machine coordinates
 * mp_set_runtime_work_offset()       - set offsets in the MR struct
 * mp_get_runtime_work_position()     - returns current axis position in work coordinates
 *                                      that were in effect at move planning time
 */

void mp_zero_segment_velocity() { mr.segment_velocity = 0;}
float mp_get_runtime_velocity(void) { return (mr.segment_velocity);}
float mp_get_runtime_absolute_position(uint8_t axis) { return (mr.position[axis]);}
void mp_set_runtime_work_offset(float offset[]) { copy_vector(mr.gm.work_offset, offset);}
float mp_get_runtime_work_position(uint8_t axis) { return (mr.position[axis] - mr.gm.work_offset[axis]);}

/*
 * mp_get_runtime_busy() - returns TRUE if motion control busy (i.e. robot is moving)
 * mp_runtime_is_idle() - returns TRUE is steppers are not actively moving
 *
 *	Use mp_get_runtime_busy() to sync to the queue. If you wait until it returns
 *	FALSE you know the queue is empty and the motors have stopped.
 */
bool mp_get_runtime_busy()
{
/*
    if ((st_runtime_isbusy() == true) ||
        (mr.move_state == MOVE_RUN) ||
        (mb.planner_state != PLANNER_IDLE)) {
        return (true);
    }
*/
    if (cm.cycle_state == CYCLE_OFF) {
        return (false);
    }
    if ((st_runtime_isbusy() == true) ||
        (mr.move_state == MOVE_RUN) ||
        (mb.planner_state == PLANNER_STARTUP)) {
        return (true);
    }
	return (false);
}

bool mp_runtime_is_idle()
{
    return (!st_runtime_isbusy());
}

/****************************************************************************************
 * mp_aline() - plan a line with acceleration / deceleration
 *
 *	This function uses constant jerk motion equations to plan acceleration and deceleration
 *	The jerk is the rate of change of acceleration; it's the 1st derivative of acceleration,
 *	and the 3rd derivative of position. Jerk is a measure of impact to the machine.
 *	Controlling jerk smooths transitions between moves and allows for faster feeds while
 *	controlling machine oscillations and other undesirable side-effects.
 *
 * 	Note All math is done in absolute coordinates using single precision floating point (float).
 *
 *	Note: Returning a status that is not STAT_OK means the endpoint is NOT advanced. So lines
 *	that are too short to move will accumulate and get executed once the accumulated error
 *	exceeds the minimums.
 */

stat_t mp_aline(GCodeState_t *gm_in)
{
	mpBuf_t *bf; 						// current move pointer
	float axis_square[AXES] = {0, 0, 0, 0, 0, 0};
	float axis_length[AXES];
	bool flags[AXES];
	float length_square = 0;
    float length;

	for (uint8_t axis=0; axis<AXES; axis++) {
		axis_length[axis] = gm_in->target[axis] - mm.position[axis];
        if ((flags[axis] = fp_NOT_ZERO(axis_length[axis]))) {   // yes, this supposed to be = not ==
            axis_square[axis] = square(axis_length[axis]);
		    length_square += axis_square[axis];
        } else {
            axis_length[axis] = 0;  // make it truly zero if it was tiny
        }
	}
	length = sqrt(length_square);

	// exit if the move has zero movement. At all.
	if (fp_ZERO(length)) {
		sr_request_status_report(SR_REQUEST_TIMED_FULL);    // Was SR_REQUEST_IMMEDIATE_FULL
		return (STAT_MINIMUM_LENGTH_MOVE);
	}

    // get a cleared buffer and copy in the Gcode model state
    if ((bf = mp_get_write_buffer()) == NULL) {             // never supposed to fail
        return(cm_panic(STAT_FAILED_GET_PLANNER_BUFFER, "aline()"));
    }
	memcpy(&bf->gm, gm_in, sizeof(GCodeState_t));

    // setup the buffer
    bf->bf_func = mp_exec_aline;                            // register the callback to the exec function
    bf->length = length;                                    // record the length
    for (uint8_t axis=0; axis<AXES; axis++) {               // compute the unit vector and set flags
        if ((bf->axis_flags[axis] = flags[axis])) {         // yes, this is supposed to be = and not ==
            bf->unit[axis] = axis_length[axis] / length;    // nb: bf-> unit was cleared by mp_get_write_buffer()
        }
    }
    _calculate_jerk(bf);                                    // compute bf->jerk values
    _calculate_vmaxes(bf, axis_length, axis_square);        // compute cruise_vmax and absolute_vmax
    _calculate_junction_vmax(bf);                           // compute maximum junction velocity constraint
    bf->entry_vmax = bf->junction_vmax;                     // provisionally set entry value for optimistic planning

    _set_diagnostics(bf); //+++++DIAGNOSTIC

	// Note: these next lines must remain in exact order. Position must update before committing the buffer.
	copy_vector(mm.position, bf->gm.target);                // set the planner position
	mp_commit_write_buffer(MOVE_TYPE_ALINE);                // commit current block (must follow the position update)
	return (STAT_OK);
}

/*
 * mp_plan_block_list() - plan all the blocks in the list
 *
 *  This parent function is just a dispatcher that reads forward in the list
 *  (towards the newest block) and calls the block planner as needed.
 *
 *	mp_plan_block_list() plans blocks starting at the planning block (p) and continuing
 *  until there are no more blocks to plan (see discussion of optimistic and pessimistic
 *  planning in planner.cpp/mp_plan_buffer(). The planning pass may be planning moves for
 *  the first time, or replanning moves, or any combination. Starting "early" will cause
 *  a replan, which is useful for feedholds and feed overrides.
 */

void mp_plan_block_list()
{
//    debug_pin1=1;      //+++++
    mpBuf_t *bf = mb.p;
    bool planned_something = false;

    while (true) {
        if ((mb.planner_state == PLANNER_OPTIMISTIC) &&         // skip last block if optimistic
            (bf->nx->buffer_state == MP_BUFFER_EMPTY)) {
            break;
        }
        if (bf->buffer_state == MP_BUFFER_EMPTY) {              // unconditional exit condition
            break;
        }
        // OK to replan running buffer during feedhold, but no other times (not supposed to happen)
        if ((cm.hold_state == FEEDHOLD_OFF) &&  (bf->buffer_state == MP_BUFFER_RUNNING)) {
            mb.p = mb.p->nx;
            return;
        }

        // plan the block
//        debug_pin2=1;               //+++++
        bf = _plan_block(bf);
        planned_something = true;
        mb.p = bf;                  //+++++ DIAGNOSTIC - put it here for debugging purposes
//        debug_pin2=0;               //+++++
    }
    if (planned_something && (cm.hold_state != FEEDHOLD_HOLD)) {
        st_request_exec_move();                     // start motion if runtime is not already busy
    }
    mb.p = bf;                                      // set planner pointer for exit
//    debug_pin1=0;                   //+++++
}

/*
 * _plan_block() - plan the current block
 *
 *  _plan_block() is called to plan each block. It typically plans forward but may
 *  backtrack to plan decelerations. It returns a pointer to the next block to be planned.
 *
 *  _plan_block() first determines if the block is an acceleration, a cruise (no change
 *  in velocity), or a deceleration, or some combination. It then sets the entry, exit and
 *  cruise velocities. It then calls trapezoid generation to set the achievable target
 *  velocity, head, body and tail lengths and times for the block.
 *
 *  Planning always occurs in the forward direction (towards nx) unless it's determined
 *  that it must backtrack to generate decelerations when needed. Backtracking always
 *  occurs at the end of move sequences (planning to zero, or the "tail"), and may
 *  occur within a move sequence if decelerations are present and severe enough to
 *  require backplanning.
 *
 *	Variable Usage:
 *    bf is the current buffer pointer, which is initialized to mb.p.
 *      The pointer changes forward or backward as blocks are planned.
 *
 *  Variables used as constants: MUST be set (typically by aline()) before calling:
 *    bf->move_type
 *    bf->length
 *    ... all the vmax's except delta_vmax
 *    ... all the jerk terms
 *
 *  Variables that may be set or change:
 *    bf->hint              - block hinting for trapezoid generation
 *    bf->delta_vmax	    - computed as needed (sparingly)
 *    bf->entry_velocity
 *    bf->exit_velocity
 *    bf->cruise_velocity
 *    bf->head_length
 *    bf->body_length
 *    bf->tail_length
 *    bf->head_time
 *    bf->body_time
 *    bf->tail_time
 *    bf->move_time
 */
static mpBuf_t *_plan_block(mpBuf_t *bf)
{
    mpBuf_t *bf_ret = mp_get_next_buffer(bf);           // buffer to return

    // Set entry_vmax and test if entry velocity can't be met by the exit velocity of
    // the previous block. If this is true, reposition to the previous block so it can
    // be corrected. This should almost never happen.
    bf->entry_vmax = bf->junction_vmax;                 // initialize entry_vmax
    if (VELOCITY_LT(bf->entry_vmax, bf->pv->exit_velocity)) {
        ASCII_ART("<");
        return (mp_get_prev_buffer(bf));                // back the planner up one
    }

    // Set the cruise and entry velocities and calculate throttling (if needed)
    bf->entry_velocity = min(bf->pv->exit_velocity, bf->cruise_vmax);
    bf->cruise_velocity = bf->cruise_vmax;              // vmax was computed in _calculate_vmaxes()
    _calculate_override(bf);                            // adjust cruise velocity for feed/traverse override
    _calculate_throttle(bf);                            // adjust cruise velocity for throttle factor

    if (bf->cruise_velocity < bf->entry_velocity) {     // adjust for a case that can happen during override or throttle
        bf->cruise_velocity = bf->entry_velocity;
    }
    //+++++ TRAPS
//    if (bf->entry_velocity > bf->cruise_velocity) { while(1); }
//    if (bf->exit_velocity > bf->cruise_velocity) { while(1); }

    if ((mb.planner_state == PLANNER_OPTIMISTIC) && !mb.backplanning) {
        bf->nx->entry_velocity = bf->cruise_velocity;   // provisionally set next block w/resulting cruise velocity
    }
    bf->exit_vmax = (bf->gm.path_control == PATH_EXACT_STOP) ? 0 : bf->cruise_velocity; // set for exact stops

    // Set the exit velocity. Choose the minimum of the exit_vmax or the entry velocity of
    // the nx block. If the nx block has already been planned use its actual entry_velocity,
    // otherwise this is invalid and the entry_vmax should be used. If next block is EMPTY
    // this expression will set bf->exit_velocity to zero.
    bf->exit_velocity = min(bf->exit_vmax, (bf->nx->buffer_state == MP_BUFFER_NOT_PLANNED) ?
                                            bf->nx->entry_vmax : bf->nx->entry_velocity);

    // Test for a perfect cruise. This allows skipping the delta_vmax computation
    if (VELOCITY_EQ(bf->cruise_velocity, bf->entry_velocity) && // this test fails more often than...
        VELOCITY_EQ(bf->cruise_velocity, bf->exit_velocity)) {  // ...this test
        bf->hint = PERFECT_CRUISE;
        ASCII_ART("-");

    } else {    // Test if delta(Ve,Vx) exceeds jerk & adjust Vx or Ve if so

         // Test acceleration cases  (Note: if Vx is decreased the nx block will be corrected in the next pass)
        if (bf->entry_velocity <= bf->exit_velocity) {

            bf->delta_vmax = mp_get_target_velocity(bf->entry_velocity, bf->length, bf);  // dV is limited by jerk
            if (VELOCITY_LT(bf->delta_vmax, (bf->exit_velocity - bf->entry_velocity))) {  // accel would exceed jerk
                bf->exit_velocity = bf->entry_velocity + bf->delta_vmax;                  // adjust Vx downward
                bf->hint = PERFECT_ACCEL;
            } else {
                bf->hint = MIXED_ACCEL;
            }
            ASCII_ART("/");

        } else { // Deceleration cases (may require back planning)

            // There are 3 cases:
            //  (1) decel is a natural slow-down or stop in an otherwise continuous movement
            //  (2) decel is a stop at the end of the buffer (a tail) & this is first it's been seen
            //  (3) decel is part of a tail (continuation of #2)
            //
            //  Want to plan (1) and (3), but defer planning (2) in case it's not a real tail
            //  Want to plan (2) to a tail if it's determined that it really is a tail
            //  Case (2) can be detected if the nx buffer is EMPTY.
            //  Case (3) can be treated the same as case (1) as a backplanning region

            // Case 2: skip backplanning
            if ((bf->nx->buffer_state == MP_BUFFER_EMPTY) && (mb.planner_state == PLANNER_OPTIMISTIC)) {
                ASCII_ART(".");
                return (mp_get_next_buffer(bf));        // return the empty buffer (break; condition)
            }
            // Case 1 & 3: start or continue a backplanning region
            if (!mb.backplanning) {
                mb.backplanning = true;                 // signal that back planning is occurring
                mb.backplan_return = bf->nx;            // return to the next buffer after start of backplan
            }
            bf->delta_vmax = mp_get_target_velocity(bf->exit_velocity, bf->length, bf);   // dV is limited by jerk

            if (VELOCITY_LT(bf->delta_vmax, (bf->entry_velocity - bf->exit_velocity))) {  // decel would exceed jerk
                bf->entry_velocity = bf->exit_velocity + bf->delta_vmax;                  // adjust Ve upward
                bf_ret = mp_get_prev_buffer(bf);
                bf->hint = PERFECT_DECEL;
            } else {
                mb.backplanning = false;
                bf_ret = mb.backplan_return;
                bf->hint = MIXED_DECEL;
            }
            ASCII_ART("\\");
        }
    }
    //+++++ TRAPS
    if (bf->entry_velocity > bf->cruise_velocity) { while(1); }
    if (bf->exit_velocity > bf->cruise_velocity) { while(1); }

    mp_calculate_trapezoid(bf);

    //+++++ TRAPS
    if (bf->entry_velocity > bf->cruise_velocity) { while(1); }
    if (bf->exit_velocity > bf->cruise_velocity) { while(1); }
    if (bf->head_length > 0.0 && bf->head_time < 0.000001) { while(1); }

    bf->buffer_state = MP_BUFFER_PLANNED;
    _set_diagnostics(bf);   //+++++ DIAGNOSTIC - need to call a function to get GCC pragmas right
    return (bf_ret);
}

/***** ALINE HELPERS *****
 * _calculate_override()
 * _calculate_throttle()
 * _calculate_jerk()
 * _calculate_vmaxes()
 * _calculate_junction_vmax()
 */

static void _calculate_override(mpBuf_t *bf)     // execute ramp to adjust cruise velocity
{
    // pull in override factor from previous block or seed initial value from the system setting
    bf->mfo_factor = fp_ZERO(bf->pv->mfo_factor) ? cm.gmx.mfo_factor : bf->pv->mfo_factor;

    // generate ramp term is a ramp is active
    if (mb.ramp_active) {
        bf->mfo_factor += mb.ramp_dvdt * bf->move_time;
        if (mb.ramp_dvdt > 0) {                             // positive is an acceleration ramp
            if (bf->mfo_factor > mb.ramp_target) {
                bf->mfo_factor = mb.ramp_target;
                mb.ramp_active = false;                     // detect end of ramp
            }
            bf->cruise_velocity *= bf->mfo_factor;
            if (bf->cruise_velocity > bf->absolute_vmax) {  // test max cruise_velocity
                bf->cruise_velocity = bf->absolute_vmax;
                mb.ramp_active = false;                     // don't allow exceeding absolute_vmax
            }
      } else {                                              // negative is deceleration ramp
            if (bf->mfo_factor < mb.ramp_target) {
                bf->mfo_factor = mb.ramp_target;
                mb.ramp_active = false;
            }
            bf->cruise_velocity *= bf->mfo_factor;      // +++++ this is probably wrong
        //  bf->exit_velocity *= bf->mfo_factor;        //...but I'm not sure this is right,
        //  bf->cruise_velocity = bf->entry_velocity;   //...either
        }
    } else {
        bf->cruise_velocity *= bf->mfo_factor;           // apply original or changed factor
    }
    // Correction for velocity constraints
    // In the case of a acceleration these conditions must hold:
    //      Ve < Vc = Vx
    // In the case of a deceleration:
    //      Ve = Vc > Vx
    // in the case of "lump":
    //      Ve < Vc > Vx
    // if (bf->cruise_velocity < bf->entry_velocity) { // deceleration case
    //     bf->cruise_velocity = bf->entry_velocity;
    // } else {                                        // acceleration case
    //     ...
    // }
}

/*
 * _calculate_throttle() - perform pro-active velocity throttling to prevent planner starvation
 *
 *  Planner throttling is needed when the arrival rate of new blocks (moves) cannot keep up
 *  with the service rate of the blocks (i.e. how fast they are removed by the runtime).
 *  For example, it is possible to receive a series of blocks that take only the minimum
 *  block time to execute; i.e. they represent about 0.75 milliseconds of machine motion each.
 *  If the average arrival and processing time for new blocks is about 4 ms (as is typical),
 *  the planner will starve. The solution is to preemptively slow down the "fast" blocks so
 *  that the service rate and arrival rate are matched and the planner does not starve.
 *  This necessarily limits the top speed the planner can achieve, but is far preferable to
 *  "stuttering". This rate-limiting is what throttling does. Looked at another way, throttling
 *  is an "automatic gain control" circuit (AGC) for the planner, and the AGC literature offers
 *  some insight as to how throttling should work.
 *
 *  These three cases illustrate the main scenarios encountered in common Gcode files:
 *
 *  Case 1: A single block arrives that is less than the minimum block time. I.e. at the
 *          requested speed it would take less than 0.75 ms to execute. This is dealt with
 *          during calculate_vmaxes() by setting the block to minimum to ensure no blocks
 *          are unplannable.
 *
 *  Case 2: A burst of blocks arrive that are less than the average arrival rate, causing
 *          the planner queue to empty faster than it can be supplied with new blocks.
 *          However, since the burst is not sustained the planner queue can compensate
 *          because averaged over time will not get to critically low levels (or starve).
 *
 *  Case 3: A prolonged series of blocks arrive that are less than the average arrival rate,
 *          causing the planner queue to starve. (Cases 2 and 3 are really just a matter of
 *          degree, but are worth examining separately.)
 *
 *  Algorithm:
 *
 *  Blocks are labeled with their expected execution time (move_time), and the entire queue
 *  is divided into regions by summing these times to get time-in-plan (Tplan).
 *  In the ASCII-art below the block that is currently running is on the left
 *  and new blocks are added to the right:
 *
 *    RUN |-------------|------------------------------------|---------------------> NEW_BLOCK
 *        Tplan (~0)    Tcritical (eg. 20ms)                 Tthrottle (eg. 100ms)
 *
 *    - Tplan is 0 at the running block. Actually, the time in the runtime is also accounted
 *      for, so Tplan at the run block is usually a few ms > 0 (and sometimes way larger).
 *
 *    - If Tplan is less than Tcritical then the planner is in imminent danger of starving.
 *      It's worth noting that this case will always occur at the end of normal motion,
 *      (during pessimistic planning) and may occur normally in other cases. If the planner
 *      is pessimistic it's required to move through the throttle and critical regions
 *      without throttling, as you may actually be stopping.
 *
 *    - If Tplan is between Tcritical and Tthrottle the planner should slow down the moves
 *      (and the resulting Tplan) to prevent the end of the queue from entering the critical
 *      region. It does this by applying an adaptive throttle_factor based on the value of
 *      Tplan, computed as so:
 *
 *                                                      -------(Y=1, no throttling)---------
 *                                                 -----
 *                                            -----
 *                                       -----          (this is actually a straight line)
 *                                  -----
 *                             -----
 *                       -----
 *     -----------------     throttle minimum factor B, e.g. B = 0.15
 *    RUN |-------------|-----------------------------------|---------------------> NEW_BLOCK
 *        Tplan (~0)    Tcritical (eg. 20ms)                 Tthrottle (eg. 80ms)
 *
 *    Y = MX + B. where:
 *      M = slope = (1-B)/(Tthrottle - Tcritical)  (NB: this is a constant)
 *      X = time_in_throttle_region = Tplan - Tcritical
 *      B = intercept = minimum throttle factor
 *      Y = the resulting override factor to adjust move velocity
 */

static void _calculate_throttle(mpBuf_t *bf)
{
    if ((bf->move_type == MOVE_TYPE_ALINE) && (mb.time_in_plan > EPSILON)) {
        if (mb.time_in_plan < PLANNER_THROTTLE_TIME) {
            bf->throttle = (THROTTLE_SLOPE * (mb.time_in_plan-PLANNER_CRITICAL_TIME) + THROTTLE_INTERCEPT);
            bf->cruise_velocity *= max(THROTTLE_MIN, bf->throttle);
        } else {
            bf->throttle = THROTTLE_MAX;    // set to 1.00 in case it's needed for backplanning
        }
        // Correction for velocity constraints
        // if (bf->cruise_velocity < bf->entry_velocity) { // deceleration case
        //     bf->cruise_velocity = bf->entry_velocity;
        // } else {                                        // acceleration case
        //     ...
        // }
    }
}
/* END NOTES:
 * It's also possible to perform throttling by tracking arrival and service rate directly, although
 * this is a predictor of starvation and not the actual starvation itself. The advantage is that
 * you can tell earlier that starvation may occur, and apply a throttle earlier. Some useful code:

 * _estimate_arrival_rate()
 *  This is pretty straightforward except for handling new_block timeouts, which you don't
 *  want to average as they are not representative of the normal incoming flow rate.

static void _estimate_arrival_rate()
{
    float arrival_time = (float)SysTickTimer_getValue() * (1/60000.0);           // convert to minutes

    // throw away samples if the buffer is blocked or blocks are not arriving (timing out)
    if (mp_planner_is_full() || (arrival_time > (mb.prev_arrival_time + NEW_BLOCK_TIMEOUT_TIME))) {
        mb.prev_arrival_time = arrival_time;
        return;
    }
    // accumulate arrival time statistics. #define ARRIVAL_RATE_SAMPLES 4, for example
    // (note: move_time_avg can be computed similarly)
    mb.arrival_rate_avg = (mb.arrival_rate_avg * ((ARRIVAL_RATE_SAMPLES-1) / ARRIVAL_RATE_SAMPLES)) +
                          ((arrival_time - mb.prev_arrival_time) * (1/ARRIVAL_RATE_SAMPLES));
    mb.prev_arrival_time = arrival_time;
}
*/

/*
 * _calculate_jerk() - calculate jerk given the dynamic state
 *
 *  Set the jerk scaling to the lowest axis with a non-zero unit vector.
 *  Go through the axes one by one and compute the scaled jerk, then pick
 *  the highest jerk that does not violate any of the axes in the move.
 *
 * Cost about ~65 uSec
 */

static void _calculate_jerk(mpBuf_t *bf)
{
    // compute the jerk as the largest jerk that still meets axis constraints
    bf->jerk = 8675309;                         // a ridiculously large number
    float jerk=0;

    for (uint8_t axis=0; axis<AXES; axis++) {
        if (fabs(bf->unit[axis]) > 0) {         // if this axis is participating in the move
            jerk = cm.a[axis].jerk_max / fabs(bf->unit[axis]);
            if (jerk < bf->jerk) {
                bf->jerk = jerk;
//              bf->jerk_axis = axis;           // +++ diagnostic
            }
        }
    }
    bf->jerk *= JERK_MULTIPLIER;                // goose it!
    bf->jerk_sq = bf->jerk * bf->jerk;          // pre-compute terms used multiple times during planning
    bf->recip_jerk = 1/bf->jerk;
}

/*
 * _calculate_vmaxes() - compute cruise_vmax and absolute_vmax based on velocity constraints
 *
 *	The following feeds and times are compared and the longest (slowest velocity) is returned:
 *	  -	G93 inverse time (if G93 is active)
 *	  -	time for coordinated move at requested feed rate
 *	  -	time that the slowest axis would require for the move
 *
 *	bf->move_time corresponds to bf->cruise_vmax and is either the velocity resulting from
 *  the requested feed rate or the fastest possible (minimum time) if the requested feed
 *  rate is not achievable. Move times for traverses are always the minimum time.
 *
 *	bf->absolute_vmax is the fastest the move can be executed given the velocity constraints
 *  on each participating axis - regardless of the feed rate requested. The minimum time /
 *  absolute_vmax is the time limited by the rate-limiting axis. It is saved for possible
 *  use later in feed override computation.
 *
 *  Velocities may be also be degraded (slowed down) if:
 *    - The block calls for a time that is less than the minimum update time (min segment time).
 *      This is very important to ensure proper block planning and trapezoid generation.
 *
 *	Prerequisites for calling this function:
 *    - Targets must be set via cm_set_target(). Axis modes are taken into account by this.
 *    - The unit vector and associated flags were computed.
 */
/* --- NIST RS274NGC_v3 Guidance ---
 *
 *	The following is verbatim text from NIST RS274NGC_v3. As I interpret A for moves that
 *	combine both linear and rotational movement, the feed rate should apply to the XYZ
 *	movement, with the rotational axis (or axes) timed to start and end at the same time
 *	the linear move is performed. It is possible under this case for the rotational move
 *	to rate-limit the linear move.
 *
 * 	2.1.2.5 Feed Rate
 *
 *	The rate at which the controlled point or the axes move is nominally a steady rate
 *	which may be set by the user. In the Interpreter, the interpretation of the feed
 *	rate is as follows unless inverse time feed rate mode is being used in the
 *	RS274/NGC view (see Section 3.5.19). The canonical machining functions view of feed
 *	rate, as described in Section 4.3.5.1, has conditions under which the set feed rate
 *	is applied differently, but none of these is used in the Interpreter.
 *
 *	A. 	For motion involving one or more of the X, Y, and Z axes (with or without
 *		simultaneous rotational axis motion), the feed rate means length units per
 *		minute along the programmed XYZ path, as if the rotational axes were not moving.
 *
 *	B.	For motion of one rotational axis with X, Y, and Z axes not moving, the
 *		feed rate means degrees per minute rotation of the rotational axis.
 *
 *	C.	For motion of two or three rotational axes with X, Y, and Z axes not moving,
 *		the rate is applied as follows. Let dA, dB, and dC be the angles in degrees
 *		through which the A, B, and C axes, respectively, must move.
 *		Let D = sqrt(dA^2 + dB^2 + dC^2). Conceptually, D is a measure of total
 *		angular motion, using the usual Euclidean metric. Let T be the amount of
 *		time required to move through D degrees at the current feed rate in degrees
 *		per minute. The rotational axes should be moved in coordinated linear motion
 *		so that the elapsed time from the start to the end of the motion is T plus
 *		any time required for acceleration or deceleration.
 */
static void _calculate_vmaxes(mpBuf_t *bf, const float axis_length[], const float axis_square[])
{
    float feed_time=0;              // one of: XYZ time, ABC time or inverse time. Mutually exclusive
	float max_time=0;				// time required for the rate-limiting axis
	float tmp_time=0;				// temp value used in computation
	float min_time=8675309;	        // looking for fastest possible execution (seed w/arbitrarily large number)
    float move_time;                // resulting move time

	// compute feed time for feeds and probe motion
	if (bf->gm.motion_mode != MOTION_MODE_STRAIGHT_TRAVERSE) {
		if (bf->gm.feed_rate_mode == INVERSE_TIME_MODE) {
			feed_time = bf->gm.feed_rate;	// NB: feed rate was un-inverted to minutes by cm_set_feed_rate()
			bf->gm.feed_rate_mode = UNITS_PER_MINUTE_MODE;
		} else {
            // compute length of linear move in millimeters. Feed rate is provided as mm/min
			feed_time = sqrt(axis_square[AXIS_X] + axis_square[AXIS_Y] + axis_square[AXIS_Z]) / bf->gm.feed_rate;
			// if no linear axes, compute length of multi-axis rotary move in degrees. Feed rate is provided as degrees/min
			if (fp_ZERO(feed_time)) {
				feed_time = sqrt(axis_square[AXIS_A] + axis_square[AXIS_B] + axis_square[AXIS_C]) / bf->gm.feed_rate;
			}
		}
	}
    // compute rate limits and absolute maximum limit
	for (uint8_t axis = AXIS_X; axis < AXES; axis++) {
        if (bf->axis_flags[axis]) {
		    if (bf->gm.motion_mode == MOTION_MODE_STRAIGHT_TRAVERSE) {
			    tmp_time = fabs(axis_length[axis]) / cm.a[axis].velocity_max;
//			    tmp_time = fabs(axis_length[axis]) * cm.a[axis].recip_velocity_max;     // remove this optimization once we have hw FPU
		    } else{//gm.motion_mode == MOTION_MODE_STRAIGHT_FEED
			    tmp_time = fabs(axis_length[axis]) / cm.a[axis].feedrate_max;
//			    tmp_time = fabs(axis_length[axis]) * cm.a[axis].recip_feedrate_max;     // the same
		    }
		    max_time = max(max_time, tmp_time);

		    if (tmp_time > 0) { 	                    // collect minimum time if this axis is not zero
			    min_time = min(min_time, tmp_time);
		    }
        }
	}
    move_time = max3(feed_time, max_time, MIN_SEGMENT_TIME);
    min_time = max(min_time, MIN_SEGMENT_TIME);
    bf->cruise_vmax = bf->length / move_time;       // target velocity requested
    bf->absolute_vmax = bf->length / min_time;      // absolute velocity limit
    bf->move_time = move_time;                      // initial estimate - used for ramp computations
}

/*
 * _calculate_junction_vmax() - Giseburt's Algorithm ;-)
 *
 *  Computes the maximum allowable junction speed by finding the velocity that will not
 *  violate the jerk value of any axis.
 *
 *  In order to achieve this we take the difference of the unit vectors of the two moves
 *  of the corner, at the point from vector a to vector b. The unit vectors of those two
 *  moves are provided as the current block (a_unit) and previous block (b_unit).
 *
 *      Delta[i]       = (b_unit[i] - a_unit[i])                   (1)
 *
 *  We take, axis by axis, the difference in "unit velocity" to get a vector that
 *  represents the direction of acceleration - which may be the opposite direction
 *  as that of the "a" vector to achieve deceleration. To get the actual acceleration,
 *  we use the corner velocity (what we intend to calculate) as the magnitude.
 *
 *      Acceleration[i] = UnitAccel[i] * Velocity[i]               (2)
 *
 *  Since we need the jerk value, which is defined as the "rate of change of acceleration,
 *  that is, the derivative of acceleration with respect to time" (Wikipedia), we need to
 *  have a quantum of time where the change in acceleration is actually carried out by the
 *  physics. That will give us the time over which to "apply" the change of acceleration
 *  in order to get a physically realistic jerk. The yields a fairly simple formula:
 *
 *      Jerk[i] = Acceleration[i] / Time                           (3)
 *
 *  Now that we can compute the jerk for a given corner, we need to know the maximum
 *  velocity that we can take the corner without violating that jerk for any axis.
 *  Let's incorporate formula (2) into formula (3), and solve for Velocity, using
 *  the known max Jerk and UnitAccel for this corner:
 *
 *      Velocity[i] = (Jerk[i] * Time) / UnitAccel[i]              (4)
 *
 *  We then compute (4) for each axis, and use the smallest (most limited) result or
 *  vmax, whichever is smaller.
 */
/* Note 1:
 *  "junction_aggression" is the integration Time quantum expressed in minutes.
 *  This is roughly on the order of 1 DDA clock tick to integrate jerk to acceleration.
 *  This is a very small number, so we multiply JA by 1,000,000 for entry and display.
 *  A reasonable JA is therefore between 0.10 and 1.0.
 *
 *  In formula 4 the jerk is multiplied by 1,000,000 and JA is divided by 1,000,000,
 *  so those terms cancel out.
 *
 *  Cost ~65 uSec
 */

static void _calculate_junction_vmax(mpBuf_t *bf)
{
    float velocity = bf->absolute_vmax;    // start with our maximum possible velocity

    for (uint8_t axis=0; axis<AXES; axis++) {
        if (bf->axis_flags[axis] || bf->pv->axis_flags[axis]) {         // skip axes with no movement
            float delta = fabs(bf->pv->unit[axis] - bf->unit[axis]);    // formula (1)

            // Corner case: If an axis has zero delta, we might have a straight line.
            // Corner case: An axis doesn't change (and it's not a straight line).
            //   In either case, division-by-zero is bad, m'kay?
            if (delta > EPSILON) {
                // formula (4): (See Note 1, above)
                velocity = min(velocity, (cm.a[axis].max_junction_accel / delta));
            }
        }
    }
    bf->junction_vmax = velocity;
}
