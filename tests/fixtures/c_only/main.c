/*
 * Fixture `c_only`: a C99 translation unit that uses the C API of Chameleon_API.h.
 *
 * Prints `TICK n=<k> value=<c_only_value()>` every 100 ms. `--ticks N` stops after N lines;
 * `--agent` also creates the agent, enables this exe's own module, calls the sync point every
 * iteration and destroys the agent at exit. It prints `AGENT enabled=1` once the broker accepted
 * the enable request. Without `--agent` it behaves like a plain program. `--agent-wait MS` bounds
 * how long it waits at exit for the broker's answer (default 30000).
 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "Chameleon_API.h"

static int g_tick = 0;

/* The function a later hot-reload scenario patches. */
int c_only_value(void)
{
    return 7;
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
        printf("TICK n=%d value=%d\n", g_tick, c_only_value());
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
