/*
 * Fixture `imgrel` (WP-36, ADR-0042): the driver. A C99 program; the code under test is imgrel.c, the translation unit
 * the scenario edits. This file is never patched: it reads and writes the globals of imgrel.c directly (CORE, fptab) and
 * through the functions of imgrel.c, so what it prints shows which storage the patched code uses. Every 100 ms:
 *   TICK n=<k> key=<a> count=<b> seed=<c> fp=<d> fl=<e> g3=<f> wide=<g>
 *     key    CORE.keys[77], cleared here and set by ig_mixed(1, 77)            1, after the edit 2
 *     count  CORE.count, reset here at 16 and incremented by ig_mixed(1, 77)   (n mod 16) + 1
 *     seed   ig_mixed(6, 9): sarr[9] + 1; sarr[9] was written ONCE, before     4252, after the edit 1004251
 *            the first tick, by ig_mixed(2, 9) = ig_g(9) = 4251
 *     fp     ig_mixed(7, 3): the function pointer this file stored in fptab[3]  43
 *     fl     ig_mixed(8, 2): (int)CORE.f[2], which this file set to 7.5         7
 *     g3     ig_mixed(3, 1): ig_g(2)                                            4244
 *     wide   ig_wide(9): sarr[9] + CORE.q[9]                                    4251, from n=9 on 4328; after the edit +2000000
 * A patch that used its own copies of the globals would print key=0, a count that stops, seed=1000000, fl=0, wide=2000000
 * and would call a null function pointer for fp. Options as `c_only`: `--ticks N`, `--agent`, `--agent-wait MS`.
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "Chameleon_API.h"
#include "imgrel.h"

static int g_tick = 0;

int ig_g(int p)
{
    return p + 4242;
}

static int ig_add40(int p)
{
    return p + 40;
}

/* Directory of this exe, where cml_agent.dll sits too. */
static int agent_directory(wchar_t* dir, uint32_t capacity)
{
    uint32_t length = 0;
    if (cml_get_current_module_path(dir, capacity, &length) != CML_STATUS_OK) {
        return 0;
    }
    while (length > 0 && dir[length - 1] != L'\\' && dir[length - 1] != L'/') {
        --length;
    }
    dir[length] = L'\0';
    return 1;
}

/* Reports the broker's answer once it is there. Returns 1 when the answer has arrived. */
static int report_enable(cml_agent agent, uint64_t request_id, uint32_t wait_ms)
{
    cml_reply reply;
    cml_status status;
    reply.status = 0;
    reply.message_length = 0;
    reply.message[0] = '\0';
    status = cml_wait_request(agent, request_id, wait_ms, &reply);
    if (status == CML_STATUS_TIMEOUT) {
        return 0;
    }
    if (status == CML_STATUS_OK && reply.status == 0) {
        printf("AGENT enabled=1 message=\"%s\"\n", reply.message);
    } else {
        printf("AGENT enabled=0 status=%d message=\"%s\"\n",
               status == CML_STATUS_OK ? (int)reply.status : (int)status, reply.message);
    }
    return 1;
}

int main(int argc, char** argv)
{
    long max_ticks = -1;
    uint32_t agent_wait_ms = 30000;
    int use_agent = 0;
    int enable_reported = 1;
    uint64_t enable_request = 0;
    cml_agent agent = NULL;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            max_ticks = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--agent") == 0) {
            use_agent = 1;
        } else if (strcmp(argv[i], "--agent-wait") == 0 && i + 1 < argc) {
            agent_wait_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    /* State the patched code must find: set here (old code), before any reload. */
    CORE.f[2] = 7.5f;
    fptab[3] = ig_add40;
    ig_mixed(2, 9); /* sarr[9] = ig_g(9) = 4251, written once */

    if (use_agent) {
        static wchar_t dir[32768];
        wchar_t module_path[32768];
        cml_status status = CML_STATUS_OK;
        uint32_t length = 0;
        if (!agent_directory(dir, 32768)) {
            printf("AGENT create failed status=%d\n", (int)CML_STATUS_AGENT_NOT_FOUND);
        } else {
            agent = cml_create_agent(dir, CML_MODE_IMMEDIATE, &status);
            if (agent == NULL) {
                printf("AGENT create failed status=%d\n", (int)status);
            } else if (cml_get_current_module_path(module_path, 32768, &length) == CML_STATUS_OK) {
                status = cml_enable_module(agent, module_path, CML_MODULE_ALL_IMPORTS, &enable_request);
                enable_reported = status != CML_STATUS_OK;
                if (enable_reported) {
                    printf("AGENT enabled=0 status=%d message=\"request was not queued\"\n", (int)status);
                }
            }
        }
    }

    while (max_ticks < 0 || g_tick < max_ticks) {
        int seed;
        int fp;
        int fl;
        int g3;
        long long wide;
        CORE.keys[77] = 0;
        if (CORE.count >= 16) {
            CORE.count = 0;
        }
        ig_mixed(1, 77);
        seed = ig_mixed(6, 9);
        fp = ig_mixed(7, 3);
        fl = ig_mixed(8, 2);
        g3 = ig_mixed(3, 1);
        wide = ig_wide(9);
        printf("TICK n=%d key=%d count=%d seed=%d fp=%d fl=%d g3=%d wide=%lld\n", g_tick, (int)CORE.keys[77], CORE.count, seed, fp, fl, g3,
               wide);
        ++g_tick;
        cml_sync_point(agent);
        if (!enable_reported) {
            enable_reported = report_enable(agent, enable_request, 0);
        }
        Sleep(100);
    }

    if (agent != NULL) {
        /* Tests need the answer before exit; a real application would never block like this. */
        if (!enable_reported && !report_enable(agent, enable_request, agent_wait_ms)) {
            printf("AGENT enabled=0 status=%d message=\"no answer from the broker\"\n",
                   (int)CML_STATUS_TIMEOUT);
        }
        cml_destroy_agent(agent);
    }
    return 0;
}
