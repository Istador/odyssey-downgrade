/*
 * main.c
 *
 * Copyright (c) 2020, DarkMatterCore <pabloacurielz@gmail.com>.
 *
 * This file is part of nxdumptool (https://github.com/DarkMatterCore/nxdumptool).
 *
 * nxdumptool is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nxdumptool is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "nxdt_utils.h"
#include "bktr.h"
#include "gamecard.h"
#include "usb.h"
#include "title.h"
#include "pfs.h"
#include "romfs.h"

#define RESET   "\033[0m"
#define BLACK   "\033[30m"      /* Black */
#define RED     "\033[31m"      /* Red */
#define GREEN   "\033[32m"      /* Green */
#define YELLOW  "\033[33m"      /* Yellow */
#define BLUE    "\033[34m"      /* Blue */
#define MAGENTA "\033[35m"      /* Magenta */
#define CYAN    "\033[36m"      /* Cyan */
#define WHITE   "\033[37m"      /* White */
#define BOLDBLACK   "\033[1m\033[30m"      /* Bold Black */
#define BOLDRED     "\033[1m\033[31m"      /* Bold Red */
#define BOLDGREEN   "\033[1m\033[32m"      /* Bold Green */
#define BOLDYELLOW  "\033[1m\033[33m"      /* Bold Yellow */
#define BOLDBLUE    "\033[1m\033[34m"      /* Bold Blue */
#define BOLDMAGENTA "\033[1m\033[35m"      /* Bold Magenta */
#define BOLDCYAN    "\033[1m\033[36m"      /* Bold Cyan */
#define BOLDWHITE   "\033[1m\033[37m"      /* Bold White */

#define BLOCK_SIZE  USB_TRANSFER_BUFFER_SIZE

bool g_borealisInitialized = false;

static PadState g_padState = {0};

static Mutex g_fileMutex = 0;
static CondVar g_readCondvar = 0, g_writeCondvar = 0;

typedef struct
{
    FILE *fd;
    NcaFsSectionContext* section_ctx;

    void *data;
    size_t data_size;
    size_t data_written;

    size_t total_offset;
    size_t total_size;

    bool read_error;
    bool write_error;
    bool transfer_cancelled;
    bool mode;
} ThreadSharedData;

static void utilsScanPads(void)
{
    padUpdate(&g_padState);
}

static u64 utilsGetButtonsDown(void)
{
    return padGetButtonsDown(&g_padState);
}

static u64 utilsGetButtonsHeld(void)
{
    return padGetButtons(&g_padState);
}

static void utilsWaitForButtonPress(u64 flag)
{
    /* Don't consider stick movement as button inputs. */
    if (!flag) flag = ~(HidNpadButton_StickLLeft | HidNpadButton_StickLRight | HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickRLeft | HidNpadButton_StickRRight | \
                        HidNpadButton_StickRUp | HidNpadButton_StickRDown);

    while(appletMainLoop())
    {
        utilsScanPads();
        if (utilsGetButtonsDown() & flag) break;
    }
}

static void consolePrint(const char *text, ...)
{
    va_list v;
    va_start(v, text);
    vfprintf(stdout, text, v);
    va_end(v);
    consoleUpdate(NULL);
}

static const char sdmc_prefix[] = "sdmc:";
static const size_t sdmc_prefix_length = sizeof(sdmc_prefix) - 1;
static const char romfs_path[] = "sdmc:/atmosphere/contents/0100000000010000/romfs.bin";
static const char exefs_path[] = "sdmc:/atmosphere/contents/0100000000010000/exefs.nsp";
static const char patch_path[] = "sdmc:/atmosphere/exefs_patches/odyssey_100_downgrade/3CA12DFAAF9C82DA064D1698DF79CDA1.ips";

static const u8 patch_data[] = {
  0x49, 0x50, 0x53, 0x33, 0x32, 0x00, 0x53, 0x70, 0x44, 0x00, 0x04, 0x00,
  0x60, 0xB8, 0x52, 0x00, 0x53, 0x70, 0x68, 0x00, 0x04, 0x06, 0x00, 0x00,
  0x14, 0x45, 0x45, 0x4F, 0x46
};

static const size_t patch_data_size = sizeof(patch_data);

static void read_thread_func(void *arg)
{
    ThreadSharedData *shared_data = (ThreadSharedData*)arg;
    if (!shared_data || !shared_data->data || !shared_data->total_size || (!shared_data->section_ctx))
    {
        shared_data->read_error = true;
        goto end;
    }

    u8 *buf = malloc(BLOCK_SIZE);
    if (!buf)
    {
        shared_data->read_error = true;
        goto end;
    }

    /* Open file. */
    const char* path = shared_data->mode ? exefs_path : romfs_path;
    shared_data->read_error = ((shared_data->fd = fopen(path, "wb")) == NULL);
    if (shared_data->read_error)
    {
        condvarWakeAll(&g_writeCondvar);
        goto cleanup;
    }

    for(u64 offset = 0, blksize = BLOCK_SIZE; offset < shared_data->total_size; offset += blksize)
    {
        if ((shared_data->total_size - offset) < blksize) blksize = (shared_data->total_size - offset);

        /* Check if the transfer has been cancelled by the user. */
        if (shared_data->transfer_cancelled)
        {
            condvarWakeAll(&g_writeCondvar);
            break;
        }

        /* Read current file data chunk. */
        shared_data->read_error = !ncaReadFsSection(shared_data->section_ctx, buf, blksize, offset + shared_data->total_offset);
        if (shared_data->read_error)
        {
            condvarWakeAll(&g_writeCondvar);
            break;
        }

        /* Wait until the previous file data chunk has been written. */
        mutexLock(&g_fileMutex);

        if (shared_data->data_size && !shared_data->write_error) condvarWait(&g_readCondvar, &g_fileMutex);

        if (shared_data->write_error)
        {
            mutexUnlock(&g_fileMutex);
            break;
        }

        /* Copy current file data chunk to the shared buffer. */
        memcpy(shared_data->data, buf, blksize);
        shared_data->data_size = blksize;

        /* Wake up the write thread to continue writing data. */
        mutexUnlock(&g_fileMutex);
        condvarWakeAll(&g_writeCondvar);
    }

    /* Wait until the previous file data chunk has been written. */
    mutexLock(&g_fileMutex);
    if (shared_data->data_size && !shared_data->write_error) condvarWait(&g_readCondvar, &g_fileMutex);
    mutexUnlock(&g_fileMutex);

    if ((shared_data->read_error || shared_data->write_error || shared_data->transfer_cancelled) && *path) remove(path);

cleanup:
    free(buf);

end:
    threadExit();
}

static void write_thread_func(void *arg)
{
    ThreadSharedData *shared_data = (ThreadSharedData*)arg;
    if (!shared_data || !shared_data->data)
    {
        shared_data->write_error = true;
        goto end;
    }

    while(shared_data->data_written < shared_data->total_size)
    {
        /* Wait until the current file data chunk has been read */
        mutexLock(&g_fileMutex);

        if (!shared_data->data_size && !shared_data->read_error) condvarWait(&g_writeCondvar, &g_fileMutex);

        if (shared_data->read_error || shared_data->transfer_cancelled || !shared_data->fd)
        {
            mutexUnlock(&g_fileMutex);
            break;
        }

        /* Write current file data chunk */
        shared_data->write_error = (fwrite(shared_data->data, 1, shared_data->data_size, shared_data->fd) != shared_data->data_size);
        if (!shared_data->write_error)
        {
            shared_data->data_written += shared_data->data_size;
            shared_data->data_size = 0;
        }

        /* Wake up the read thread to continue reading data */
        mutexUnlock(&g_fileMutex);
        condvarWakeAll(&g_readCondvar);

        if (shared_data->write_error) break;
    }

end:
    threadExit();
}

u8 get_program_id_offset(TitleInfo *info, u32 program_count)
{
    return 0;
}

#define R_PATH_EXISTS (0x402)
#define R_PATH_DOESNT_EXIST (0x202)

void do_add_downgrade() {
    consoleClear();

    u32 app_count = 0;
    TitleApplicationMetadata **app_metadata = NULL;
    TitleUserApplicationData user_app_data = {0};

    u8 *buf = NULL;

    NcaContext *base_nca_ctx = NULL;

    ThreadSharedData shared_data = {0};
    Thread read_thread = {0}, write_thread = {0};

    app_metadata = titleGetApplicationMetadataEntries(false, &app_count);
    if (!app_metadata || !app_count)
    {
        consolePrint("app metadata failed\n");
        goto cleanup;
    }

    consolePrint("app metadata succeeded\n");

    buf = usbAllocatePageAlignedBuffer(BLOCK_SIZE);
    if (!buf)
    {
        consolePrint("buf failed\n");
        goto cleanup;
    }

    consolePrint("buf succeeded\n");

    base_nca_ctx = calloc(1, sizeof(NcaContext));
    if (!base_nca_ctx)
    {
        consolePrint("base nca ctx buf failed\n");
        goto cleanup;
    }

    consolePrint("base nca ctx buf succeeded\n");

    utilsSleep(1);

    int odysseyIdx = -1;
    for(int i = 0; i < app_count; i++) {
        TitleApplicationMetadata* meta = app_metadata[i];

        if(meta->title_id == 0x0100000000010000ull) {
            odysseyIdx = i;
            break;
        }
    }

    if(odysseyIdx == -1) {
        consolePrint("Failed to find odyssey installed.\n");
        goto cleanup;
    }

    if(!titleGetUserApplicationData(app_metadata[odysseyIdx]->title_id, &user_app_data) || !user_app_data.app_info) {
        consolePrint("Odyssey seems to not be fully installed, missing your gamecard?\n");
        goto cleanup;
    }

    u32 program_count = titleGetContentCountByType(user_app_data.app_info, NcmContentType_Program);
    if (!program_count)
    {
        consolePrint("base app has no program ncas!\n");
        goto cleanup;
    }

    u8 program_id_offset = get_program_id_offset(user_app_data.app_info, program_count);
    if (program_id_offset >= program_count)
    {
        goto cleanup;
    }

    consolePrint("selected title:\n%s (%016lX)\n\n", app_metadata[odysseyIdx]->lang_entry.name, app_metadata[odysseyIdx]->title_id + program_id_offset);

    if (!ncaInitializeContext(
      base_nca_ctx,
      user_app_data.app_info->storage_id,
      (user_app_data.app_info->storage_id == NcmStorageId_GameCard ? GameCardHashFileSystemPartitionType_Secure : 0),
      titleGetContentInfoByTypeAndIdOffset(user_app_data.app_info, NcmContentType_Program, program_id_offset),
      user_app_data.app_info->version.value,
      NULL
    ))
    {
        consolePrint("nca initialize base ctx failed\n");
        goto cleanup;
    }

    RomFileSystemContext romfs_ctx = {0};
    if (!romfsInitializeContext(&romfs_ctx, &(base_nca_ctx->fs_ctx[1]), NULL))
    {
        consolePrint("romfs initialize ctx failed\n");
        goto cleanup;
    }
    consolePrint("romfs initialize ctx succeeded\n");

    PartitionFileSystemContext exefs_ctx = {0};
    if (!pfsInitializeContext(&exefs_ctx, &(base_nca_ctx->fs_ctx[0])))
    {
        consolePrint("exefs initialize ctx failed\n");
        goto cleanup;
    }
    consolePrint("exefs initialize ctx succeeded\n");

    shared_data.section_ctx = &(base_nca_ctx->fs_ctx[1]);
    shared_data.total_offset = romfs_ctx.offset;
    shared_data.total_size = romfs_ctx.size;
    shared_data.mode = false;

    shared_data.data = buf;
    shared_data.data_size = 0;
    shared_data.data_written = 0;

    consolePrint("creating file...");

    if(!utilsCreateConcatenationFileWithSize(romfs_path, romfs_ctx.size))
    {
        consolePrint("create concatenation file failed\n");
        goto cleanup;
    }

    consolePrint("done\n");

dump_start:
    consolePrint("creating threads\n");
    utilsCreateThread(&read_thread, read_thread_func, &shared_data, 2);
    utilsCreateThread(&write_thread, write_thread_func, &shared_data, 2);

    u8 prev_time = 0;
    u64 prev_size = 0;
    u8 percent = 0;

    time_t btn_cancel_start_tmr = 0, btn_cancel_end_tmr = 0;
    bool btn_cancel_cur_state = false, btn_cancel_prev_state = false;

    utilsSetLongRunningProcessState(true);

    consolePrint("hold b to cancel\n\n");

    time_t start = time(NULL);

    while(shared_data.data_written < shared_data.total_size)
    {
        if (shared_data.read_error || shared_data.write_error) break;

        struct tm ts = {0};
        time_t now = time(NULL);
        localtime_r(&now, &ts);

        size_t size = shared_data.data_written;

        utilsScanPads();
        btn_cancel_cur_state = (utilsGetButtonsHeld() & HidNpadButton_B);

        if (btn_cancel_cur_state && btn_cancel_cur_state != btn_cancel_prev_state)
        {
            btn_cancel_start_tmr = now;
        } else
        if (btn_cancel_cur_state && btn_cancel_cur_state == btn_cancel_prev_state)
        {
            btn_cancel_end_tmr = now;
            if ((btn_cancel_end_tmr - btn_cancel_start_tmr) >= 3)
            {
                mutexLock(&g_fileMutex);
                shared_data.transfer_cancelled = true;
                mutexUnlock(&g_fileMutex);
                break;
            }
        } else {
            btn_cancel_start_tmr = btn_cancel_end_tmr = 0;
        }

        btn_cancel_prev_state = btn_cancel_cur_state;

        if (prev_time == ts.tm_sec || prev_size == size) continue;

        percent = (u8)((size * 100) / shared_data.total_size);

        prev_time = ts.tm_sec;
        prev_size = size;

        printf("%lu / %lu (%u%%) | Time elapsed: %lu\n", size, shared_data.total_size, percent, (now - start));
        consoleUpdate(NULL);
    }

    start = (time(NULL) - start);

    consolePrint("\nwaiting for threads to join\n");
    utilsJoinThread(&read_thread);
    consolePrint("read_thread done: %lu\n", time(NULL));
    utilsJoinThread(&write_thread);
    consolePrint("write_thread done: %lu\n", time(NULL));

    utilsSetLongRunningProcessState(false);

    if (shared_data.read_error || shared_data.write_error)
    {
        consolePrint("i/o error\n");
        goto cleanup;
    }

    fclose(shared_data.fd);

    if (shared_data.transfer_cancelled)
    {
        consolePrint("process cancelled\n");
        goto cleanup;
    }

    consolePrint("process completed in %lu seconds\n", start);

    if(shared_data.mode == false) {
        shared_data.section_ctx = &(base_nca_ctx->fs_ctx[0]);
        shared_data.total_offset = exefs_ctx.offset;
        shared_data.total_size = exefs_ctx.size;
        shared_data.mode = true;

        shared_data.data = buf;
        shared_data.data_size = 0;
        shared_data.data_written = 0;
        goto dump_start;
    }

    if(user_app_data.app_info->storage_id == NcmStorageId_GameCard)
        consolePrint("if odysey doesn't launch, reinsert your gamecard.\n");

cleanup:
    if (base_nca_ctx) free(base_nca_ctx);

    titleFreeUserApplicationData(&user_app_data);

    if (buf) free(buf);

    if (app_metadata) free(app_metadata);

    consolePrint("press any button to exit\n");
    utilsWaitForButtonPress(0);
}

void do_add_patch() {
    consoleClear();
    FsFileSystem* fs = utilsGetSdCardFileSystemObject();

    /* May error if file already exists, this is ok. */
    int r = fsFsCreateFile(fs, patch_path + sdmc_prefix_length, patch_data_size, 0);
    if(R_FAILED(r) && r != R_PATH_EXISTS) {
        consolePrint("failed to write patch (%x)\n", r);
        goto end;
    }

    FsFile f;
    r = fsFsOpenFile(fs, patch_path + sdmc_prefix_length, FsOpenMode_Write, &f);
    if(R_FAILED(r)) {
        consolePrint("failed to write patch (%x)\n", r);
        goto end;
    }

    r = fsFileWrite(&f, 0, patch_data, patch_data_size, FsWriteOption_Flush);
    if(R_FAILED(r)) {
        consolePrint("failed to write patch (%x)\n", r);
        fsFileClose(&f);
        goto end;
    }
    fsFileClose(&f);
    consolePrint("done\n");
end:
    consolePrint("press any button to exit\n");
    utilsWaitForButtonPress(0);
}

void do_remove_downgrade() {
    consoleClear();
    FsFileSystem* fs = utilsGetSdCardFileSystemObject();

    int r = fsFsDeleteFile(fs, romfs_path + sdmc_prefix_length);
    if(R_FAILED(r) && r != R_PATH_DOESNT_EXIST) {
        consolePrint("failed to delete romfs.bin (%x)\n", r);
        goto end;
    }

    r = fsFsDeleteFile(fs, exefs_path + sdmc_prefix_length);
    if(R_FAILED(r) && r != R_PATH_DOESNT_EXIST) {
        consolePrint("failed to delete exefs.nsp (%x)\n", r);
        goto end;
    }

    consolePrint("done\n");
end:
    consolePrint("press any button to exit\n");
    utilsWaitForButtonPress(0);
}

void do_remove_patch() {
    consoleClear();
    FsFileSystem* fs = utilsGetSdCardFileSystemObject();

    int r = fsFsDeleteFile(fs, patch_path + sdmc_prefix_length);
    if(R_FAILED(r) && r != R_PATH_DOESNT_EXIST) {
        consolePrint("failed to delete patch (%x)\n", r);
        goto end;
    }
    consolePrint("done\n");
end:
    consolePrint("press any button to exit\n");
    utilsWaitForButtonPress(0);
}

typedef enum _status_t {
    STATUS_NONE         = 0,
    STATUS_DOWNGRADE    = 1 << 0,
    STATUS_PATCH        = 1 << 1,
} status_t;


static status_t get_status() {
    status_t status = STATUS_NONE;

    if(utilsCheckIfFileExists(romfs_path) && utilsCheckIfFileExists(exefs_path))
        status |= STATUS_DOWNGRADE;

    if(utilsCheckIfFileExists(patch_path))
        status |= STATUS_PATCH;

    return status;
}

int main(int argc, char *argv[])
{
    int ret = 0;

    if (!utilsInitializeResources(argc, (const char**)argv))
    {
        ret = -1;
        goto out;
    }

    /* Configure input. */
    /* Up to 8 different, full controller inputs. */
    /* Individual Joy-Cons not supported. */
    padConfigureInput(8, HidNpadStyleSet_NpadFullCtrl);
    padInitializeWithMask(&g_padState, 0x1000000FFUL);

    consoleInit(NULL);

    utilsCreateDirectoryTree(romfs_path, false);
    utilsCreateDirectoryTree(exefs_path, false);
    utilsCreateDirectoryTree(patch_path, false);

    bool applet_status;
    int selected_idx = 0;

    #define MENU_COUNT (4)

    const char* menu_names[MENU_COUNT] = {
        "Add downgrade",
        "Add patch",
        "Remove downgrade",
        "Remove patch",
    };

    const void (*menu_funcs[MENU_COUNT])() = {
        do_add_downgrade,
        do_add_patch,
        do_remove_downgrade,
        do_remove_patch,
    };

    status_t status = get_status();

    while((applet_status = appletMainLoop()))
    {
        consoleClear();
        printf("odyssey downgrade\npress b to exit.\n\n");

        for(u32 i = 0; i < MENU_COUNT; i++)
        {
            printf("%s%s\n", i == selected_idx ? " -> " : "    ", menu_names[i]);
        }
        printf("\n");

        if(status & STATUS_DOWNGRADE) {
            printf(GREEN "Downgrade already added.\n" RESET);
        } else {
            printf(RED "Downgrade not added.\n" RESET);
        }

        if(status & STATUS_PATCH) {
            printf(GREEN "Patch already added.\n" RESET);
        } else {
            printf(RED "Patch not added.\n" RESET);
        }

        consoleUpdate(NULL);

        u64 btn_down = 0, btn_held = 0;
        while((applet_status = appletMainLoop()))
        {
            utilsScanPads();
            btn_down = utilsGetButtonsDown();
            btn_held = utilsGetButtonsHeld();
            if (btn_down || btn_held) break;
        }

        if (btn_down & HidNpadButton_A)
        {
            menu_funcs[selected_idx]();
            status = get_status();
            continue;

        } else
        if ((btn_down & HidNpadButton_Down) || (btn_held & (HidNpadButton_StickLDown | HidNpadButton_StickRDown)))
        {
            selected_idx++;

            if (selected_idx >= MENU_COUNT)
            {
                if (btn_down & HidNpadButton_Down)
                {
                    selected_idx = 0;
                } else {
                    selected_idx = (MENU_COUNT - 1);
                }
            }
        } else
        if ((btn_down & HidNpadButton_Up) || (btn_held & (HidNpadButton_StickLUp | HidNpadButton_StickRUp)))
        {
            selected_idx--;

            if (selected_idx == UINT32_MAX)
            {
                if (btn_down & HidNpadButton_Up)
                {
                    selected_idx = (MENU_COUNT - 1);
                } else {
                    selected_idx = 0;
                }
            }
        } else
        if (btn_down & HidNpadButton_B)
        {
            break;
        }

        if (btn_held & (HidNpadButton_StickLDown | HidNpadButton_StickRDown | HidNpadButton_StickLUp | HidNpadButton_StickRUp)) svcSleepThread(50000000); // 50 ms
    }

out:
    utilsCloseResources();

    consoleExit(NULL);

    return ret;
}
