#include <ultra64.h>
#include <PR/os_internal.h>
#include "config.h"
#include "string.h"
#include "controller.h"
#include "engine/math_util.h"
#include "game_init.h"
#include "game_input.h"
#include "rumble_init.h"

OSPortInfo gPortInfo[MAXCONTROLLERS] = { 0 };

void __osSiGetAccess(void);
void __osSiRelAccess(void);

////////////////////
// contreaddata.c //
////////////////////

static void __osPackReadData(void);
static u16 __osTranslateGCNButtons(GCNButtons gcn, s32 c_stick_x, s32 c_stick_y);

/**
 * @brief Sets up PIF commands to poll controller inputs.
 * Unmodified from vanilla libultra, but __osPackReadData is modified.
 * Called by poll_controller_inputs.
 */
s32 osContStartReadDataEx(OSMesgQueue* mq) {
    s32 ret = 0;

    __osSiGetAccess();

    if (__osContLastCmd != CONT_CMD_READ_BUTTON) {
        __osPackReadData();
        ret = __osSiRawStartDma(OS_WRITE, &__osContPifRam);
        osRecvMesg(mq, NULL, OS_MESG_BLOCK);
    }

    ret = __osSiRawStartDma(OS_READ, &__osContPifRam);
    __osContLastCmd = CONT_CMD_READ_BUTTON;
    __osSiRelAccess();

    return ret;
}

/**
 * @brief Reads PIF command result written by __osPackReadData and converts it into OSContPadEx data.
 * Modified from vanilla libultra to handle GameCube controllers, skip empty/unassigned ports,
 *   and trigger status polling if an active controller is unplugged.
 * Called by poll_controller_inputs.
 */
void osContGetReadDataEx(OSContPadEx* data) {
    u8* ptr = (u8*)__osContPifRam.ramarray;
    __OSContReadFormat readformat;
    __OSContGCNShortPollFormat readformatgcn;
    OSPortInfo* portInfo = NULL;
    int port;

    for (port = 0; port < __osMaxControllers; port++) {
        portInfo = &gPortInfo[port];

        if (portInfo->plugged && (gContStatusPolling || portInfo->playerNum)) {
            // If a controller being read was unplugged, start status polling on all 4 ports.
            if (CHNL_ERR((*(__OSContReadFormat*)ptr).cmd) & (CHNL_ERR_NORESP >> 4)) {
                start_controller_status_polling();
                return;
            }

            if (portInfo->type & CONT_CONSOLE_GCN) {
                OSContCenter* contCenter = &gPortInfo[port].contCenter;
                s32 stick_x, stick_y, c_stick_x, c_stick_y;
                readformatgcn = *(__OSContGCNShortPollFormat*)ptr;
                data->errno = CHNL_ERR(readformatgcn.cmd);

                if (data->errno == (CONT_CMD_RX_SUCCESSFUL >> 4)) {
                    if (!contCenter->initialized) {
                        contCenter->initialized = TRUE;
                        contCenter->stick.x     = readformatgcn.recv.input.stick.x;
                        contCenter->stick.y     = readformatgcn.recv.input.stick.y;
                        contCenter->c_stick.x   = readformatgcn.recv.input.c_stick.x;
                        contCenter->c_stick.y   = readformatgcn.recv.input.c_stick.y;
                    }

                    data->stick_x   = stick_x   = CLAMP_S8(((s32)readformatgcn.recv.input.stick.x  ) - contCenter->stick.x  );
                    data->stick_y   = stick_y   = CLAMP_S8(((s32)readformatgcn.recv.input.stick.y  ) - contCenter->stick.y  );
                    data->c_stick_x = c_stick_x = CLAMP_S8(((s32)readformatgcn.recv.input.c_stick.x) - contCenter->c_stick.x);
                    data->c_stick_y = c_stick_y = CLAMP_S8(((s32)readformatgcn.recv.input.c_stick.y) - contCenter->c_stick.y);
                    data->button    = __osTranslateGCNButtons(readformatgcn.recv.input.buttons, c_stick_x, c_stick_y);
                    data->l_trig    = readformatgcn.recv.input.l_trig;
                    data->r_trig    = readformatgcn.recv.input.r_trig;
                } else {
                    contCenter->initialized = FALSE;
                }

                ptr += sizeof(__OSContGCNShortPollFormat);
            } else {
                readformat = *(__OSContReadFormat*)ptr;
                data->errno = CHNL_ERR(readformat.cmd);

                if (data->errno == (CONT_CMD_RX_SUCCESSFUL >> 4)) {
                    data->button    = readformat.recv.input.buttons.raw;
                    data->stick_x   = readformat.recv.input.stick.x;
                    data->stick_y   = readformat.recv.input.stick.y;
                    data->c_stick_x = 0;
                    data->c_stick_y = 0;
                    data->l_trig    = 0;
                    data->r_trig    = 0;
                }

                ptr += sizeof(__OSContReadFormat);
            }
        } else {
            // Skip empty channels/ports.
            ptr++;
        }

        data++;
    }
}

/**
 * @brief Writes PIF commands to poll controller inputs.
 * Modified from vanilla libultra to handle GameCube controllers and skip empty/unassigned ports.
 * Called by osContStartReadData and osContStartReadDataEx.
 */
static void __osPackReadData(void) {
    u8* ptr = (u8*)__osContPifRam.ramarray;
    __OSContReadFormat readformat;
    __OSContGCNShortPollFormat readformatgcn;
    OSPortInfo* portInfo = NULL;
    int port;

    bzero(__osContPifRam.ramarray, sizeof(__osContPifRam.ramarray));

    __osContPifRam.pifstatus = PIF_STATUS_EXE;

    // N64 controller poll format.
    readformat.cmd.txsize                = sizeof(readformat.send);
    readformat.cmd.rxsize                = sizeof(readformat.recv);
    readformat.send.cmdID                = CONT_CMD_READ_BUTTON;
    readformat.recv.input.buttons.raw    = 0xFFFF;
    readformat.recv.input.stick.raw      = 0xFFFF;

    // GameCube controller poll format.
    readformatgcn.cmd.txsize             = sizeof(readformatgcn.send);
    readformatgcn.cmd.rxsize             = sizeof(readformatgcn.recv);
    readformatgcn.send.cmdID             = CONT_CMD_GCN_SHORT_POLL;
    // The GameCube controller has various modes for returning the lower analog bits (4-bit vs. 8-bit).
    // Mode 3 uses 8 bits for both c-stick and shoulder triggers.
    // https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/Core/HW/SI/SI_DeviceGCController.cpp
    // https://github.com/extremscorner/gba-as-controller/blob/gc/controller/source/main.iwram.c
    readformatgcn.send.analog_mode       = 3;
    readformatgcn.send.rumble            = MOTOR_STOP;
    readformatgcn.recv.input.buttons.raw = 0xFFFF;
    readformatgcn.recv.input.stick.raw   = 0xFFFF;

    for (port = 0; port < __osMaxControllers; port++) {
        portInfo = &gPortInfo[port];

        if (portInfo->plugged && (gContStatusPolling || portInfo->playerNum)) {
            if (portInfo->type & CONT_CONSOLE_GCN) {
                readformatgcn.send.rumble = portInfo->gcRumble;
                *(__OSContGCNShortPollFormat*)ptr = readformatgcn;
                ptr += sizeof(__OSContGCNShortPollFormat);
            } else {
                *(__OSContReadFormat*)ptr = readformat;
                ptr += sizeof(__OSContReadFormat);
            }
        } else {
            // Empty channel/port, so Leave a CONT_CMD_SKIP_CHNL (0x00) byte to tell the PIF to skip it.
            ptr++;
        }
    }

    *ptr = CONT_CMD_END;
}

/**
 * @brief Maps GCN input bits to N64 standard controller input bits.
 * Called by osContGetReadData and osContGetReadDataEx.
 */
static u16 __osTranslateGCNButtons(GCNButtons gcn, s32 c_stick_x, s32 c_stick_y) {
    N64Buttons n64 = { .raw = 0x0 };

    n64.standard.A       = gcn.standard.A;
    n64.standard.B       = gcn.standard.B;
    n64.standard.Z       = gcn.standard.Z;
    n64.standard.START   = gcn.standard.START;
    n64.standard.D_UP    = gcn.standard.D_UP;
    n64.standard.D_DOWN  = gcn.standard.D_DOWN;
    n64.standard.D_LEFT  = gcn.standard.D_LEFT;
    n64.standard.D_RIGHT = gcn.standard.D_RIGHT;
    n64.standard.RESET   = gcn.standard.X;
    n64.standard.unused  = gcn.standard.Y;
    n64.standard.L       = gcn.standard.L;
    n64.standard.R       = gcn.standard.R;
    n64.standard.C_UP    = (c_stick_y >  GCN_C_STICK_THRESHOLD);
    n64.standard.C_DOWN  = (c_stick_y < -GCN_C_STICK_THRESHOLD);
    n64.standard.C_LEFT  = (c_stick_x < -GCN_C_STICK_THRESHOLD);
    n64.standard.C_RIGHT = (c_stick_x >  GCN_C_STICK_THRESHOLD);

    return n64.raw;
}

/////////////////
// contquery.c //
/////////////////

void __osContGetInitDataEx(u8* pattern, OSContStatus* data);

/**
 * @brief Read status query data written by osContStartQuery.
 * odified from vanilla libultra to return bitpattern, similar to osContInit.
 * Called by poll_controller_statuses.
 */
void osContGetQueryEx(u8* bitpattern, OSContStatus* data) {
    __osContGetInitDataEx(bitpattern, data);
}

//////////////////
// controller.c //
//////////////////

/**
 * @brief Reads PIF command result written by __osPackRequestData and converts it into OSContStatus data.
 * Linker script will resolve references to the original function with this one instead.
 * Modified from vanilla libultra to set gPortInfo type and plugged status.
 * Called by osContInit, osContGetQuery, osContGetQueryEx, and osContReset.
 */
void __osContGetInitDataEx(u8* pattern, OSContStatus* data) {
    u8* ptr = (u8*)__osContPifRam.ramarray;
    __OSContRequestFormat requestHeader;
    OSPortInfo* portInfo = NULL;
    u8 bits = 0x0;
    int port;

    for (port = 0; port < __osMaxControllers; port++) {
        requestHeader = *(__OSContRequestFormat*)ptr;
        data->error = CHNL_ERR(requestHeader.cmd);

        if (data->error == (CONT_CMD_RX_SUCCESSFUL >> 4)) {
            portInfo = &gPortInfo[port];

            // Byteswap the SI identifier.
            data->type = ((requestHeader.recv.typel << 8) | requestHeader.recv.typeh);

            // Check the type of controller
            // Some mupen cores seem to send back a controller type of CONT_TYPE_NULL (0xFFFF) if the core doesn't initialize the input plugin quickly enough,
            //   so check for that and set the input type to N64 controller if so.
            portInfo->type = ((s16)data->type == (s16)CONT_TYPE_NULL) ? CONT_TYPE_NORMAL : data->type;

            // Set this port's status.
            data->status = requestHeader.recv.status;
            portInfo->plugged = TRUE;
            bits |= (1 << port);
        }

        ptr += sizeof(requestHeader);
        data++;
    }

    *pattern = bits;
}

/////////////
// motor.c //
/////////////

static OSPifRam __MotorDataBuf[MAXCONTROLLERS];

/**
 * @brief Turns controller rumble on or off.
 * Modified from vanilla libultra to handle GameCube controller rumble.
 * Called by osMotorStart, osMotorStop, and osMotorStopHard via macro.
 */
s32 __osMotorAccessEx(OSPfs* pfs, s32 flag) {
    s32 err = PFS_ERR_SUCCESS;
    int channel = pfs->channel;
    u8* ptr = (u8*)&__MotorDataBuf[channel];

    if (!(pfs->status & PFS_MOTOR_INITIALIZED)) {
        return PFS_ERR_INVALID;
    }

    if (gPortInfo[channel].type & CONT_CONSOLE_GCN) {
        gPortInfo[channel].gcRumble = flag;
        __osContLastCmd = CONT_CMD_END;
    } else {
        // N64 Controllers don't have MOTOR_STOP_HARD.
        if (flag == MOTOR_STOP_HARD) {
            flag = MOTOR_STOP;
        }

        __osSiGetAccess();
        __MotorDataBuf[channel].pifstatus = PIF_STATUS_EXE;
        ptr += channel;

        __OSContRamWriteFormat* readformat = (__OSContRamWriteFormat*)ptr;

        memset(readformat->send.data, flag, sizeof(readformat->send.data));

        __osContLastCmd = CONT_CMD_END;
        __osSiRawStartDma(OS_WRITE, &__MotorDataBuf[channel]);
        osRecvMesg(pfs->queue, NULL, OS_MESG_BLOCK);
        __osSiRawStartDma(OS_READ, &__MotorDataBuf[channel]);
        osRecvMesg(pfs->queue, NULL, OS_MESG_BLOCK);

        err = (readformat->cmd.rxsize & CHNL_ERR_MASK);
        if (!err) {
            if (!flag) {
                // MOTOR_STOP
                if (readformat->recv.datacrc != 0) {
                    err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
                }
            } else {
                // MOTOR_START
                if (readformat->recv.datacrc != 0xEB) {
                    err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
                }
            }
        }
        __osSiRelAccess();
    }

    return err;
}

u8 __osContAddressCrc(u16 addr);
s32 __osPfsSelectBank(OSPfs* pfs, u8 bank);
s32 __osContRamRead(OSMesgQueue* mq, int channel, u16 address, u8* buffer);

/**
 * @brief Writes PIF commands to control the rumble pak.
 * Unmodified from vanilla libultra.
 * Called by osMotorInit and osMotorInitEx.
 */
static void _MakeMotorData(int channel, OSPifRam* mdata) {
    u8* ptr = (u8*)mdata->ramarray;
    __OSContRamWriteFormat ramreadformat;
    int i;

    ramreadformat.align      = CONT_CMD_NOP;
    ramreadformat.cmd.txsize = sizeof(ramreadformat.send);
    ramreadformat.cmd.rxsize = sizeof(ramreadformat.recv);
    ramreadformat.send.cmdID = CONT_CMD_WRITE_MEMPAK;
    ramreadformat.send.addrh = (CONT_BLOCK_RUMBLE >> 3);
    ramreadformat.send.addrl = (u8)(__osContAddressCrc(CONT_BLOCK_RUMBLE) | (CONT_BLOCK_RUMBLE << 5));

    if (channel != 0) {
        for (i = 0; i < channel; i++) {
            *ptr++ = CONT_CMD_SKIP_CHNL;
        }
    }

    *(__OSContRamWriteFormat*)ptr = ramreadformat;
    ptr += sizeof(__OSContRamWriteFormat);
    *ptr = CONT_CMD_END;
}

/**
 * @brief Initializes the Rumble Pak.
 * Modified from vanilla libultra to ignore GameCube controllers.
 * Called by thread6_rumble_loop and cancel_rumble.
 */
s32 osMotorInitEx(OSMesgQueue* mq, OSPfs* pfs, int channel) {
    s32 err;
    u8 data[BLOCKSIZE];

    pfs->status     = PFS_STATUS_NONE;
    pfs->queue      = mq;
    pfs->channel    = channel;
    pfs->activebank = ACCESSORY_ID_NULL;

    if (!(gPortInfo[channel].type & CONT_CONSOLE_GCN)) {
        // Write probe value (ensure Transfer Pak is turned off).
        err = __osPfsSelectBank(pfs, ACCESSORY_ID_TRANSFER_OFF);
        if (err == PFS_ERR_NEW_PACK) {
            // Write probe value (Rumble bank).
            err = __osPfsSelectBank(pfs, ACCESSORY_ID_RUMBLE);
        }
        if (err != PFS_ERR_SUCCESS) {
            return err;
        }

        // Read probe value (1).
        err = __osContRamRead(mq, channel, CONT_BLOCK_DETECT, data);
        if (err == PFS_ERR_NEW_PACK) {
            err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
        }
        if (err != PFS_ERR_SUCCESS) {
            return err;
        }

        // Ensure the accessory is not a turned off Transfer Pak.
        if (data[BLOCKSIZE - 1] == ACCESSORY_ID_TRANSFER_OFF) {
            return PFS_ERR_DEVICE; // Wrong device
        }

        // Write probe value (Rumble bank).
        err = __osPfsSelectBank(pfs, ACCESSORY_ID_RUMBLE);
        if (err == PFS_ERR_NEW_PACK) {
            err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
        }
        if (err != PFS_ERR_SUCCESS) {
            return err;
        }

        // Read probe value (2).
        err = __osContRamRead(mq, channel, CONT_BLOCK_DETECT, data);
        if (err == PFS_ERR_NEW_PACK) {
            err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
        }
        if (err != PFS_ERR_SUCCESS) {
            return err;
        }

        // Ensure the accessory is a Rumble Pak.
        if (data[BLOCKSIZE - 1] != ACCESSORY_ID_RUMBLE) {
            return PFS_ERR_DEVICE; // Wrong device
        }

        // Write the PIF command.
        if (!(pfs->status & PFS_MOTOR_INITIALIZED)) {
            _MakeMotorData(channel, &__MotorDataBuf[channel]);
        }
    }

    pfs->status = PFS_MOTOR_INITIALIZED;

    return PFS_ERR_SUCCESS;
}
