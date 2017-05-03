/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "syscfg/syscfg.h"
#include "os/os.h"
#include "console/console.h"
#include "sysinit/sysinit.h"
#include "shell/shell.h"

#define SHELL_PROMPT "shell> "

#define MODULE_NAME_MAX_LEN     20

/* additional chars are "> " (include '\0' )*/
#define PROMPT_SUFFIX 3
#define PROMPT_MAX_LEN (MODULE_NAME_MAX_LEN + PROMPT_SUFFIX)

static struct shell_module shell_modules[MYNEWT_VAL(SHELL_MAX_MODULES)];
static size_t num_of_shell_entities;

static const char *prompt;
static char default_module_prompt[PROMPT_MAX_LEN];
static int default_module = -1;

static shell_cmd_func_t app_cmd_handler;
static shell_prompt_function_t app_prompt_handler;

static struct console_input buf[MYNEWT_VAL(SHELL_MAX_CMD_QUEUED)];

static struct os_eventq avail_queue;
static struct os_event shell_console_ev[MYNEWT_VAL(SHELL_MAX_CMD_QUEUED)];

static const char *
get_prompt(void)
{
    if (app_prompt_handler) {
        const char *str;

        str = app_prompt_handler();
        if (str) {
            return str;
        }
    }

    if (default_module != -1) {
        return default_module_prompt;
    }

    return prompt;
}

static size_t
line2argv(char *str, char *argv[], size_t size)
{
    size_t argc = 0;

    if (!strlen(str)) {
        return 0;
    }

    while (*str && *str == ' ') {
        str++;
    }

    if (!*str) {
        return 0;
    }

    argv[argc++] = str;

    while ((str = strchr(str, ' '))) {
        *str++ = '\0';

        while (*str && *str == ' ') {
            str++;
        }

        if (!*str) {
            break;
        }

        argv[argc++] = str;

        if (argc == size) {
            console_printf("Too many parameters (max %zu)\n", size - 1);
            return 0;
        }
    }

    /* keep it POSIX style where argv[argc] is required to be NULL */
    argv[argc] = NULL;

    return argc;
}

static int
get_destination_module(const char *module_str, uint8_t len)
{
    int i;

    for (i = 0; i < num_of_shell_entities; i++) {
        if (!strncmp(module_str,
                     shell_modules[i].module_name, len)) {
            return i;
        }
    }

    return -1;
}

/* For a specific command: argv[0] = module name, argv[1] = command name
 * If a default module was selected: argv[0] = command name
 */
static const char *
get_command_and_module(char *argv[], int *module)
{
    *module = -1;

    if (!argv[0]) {
        console_printf("Unrecognized command\n");
        return NULL;
    }

    if (default_module == -1) {
        if (!argv[1] || argv[1][0] == '\0') {
            console_printf("Unrecognized command: %s\n", argv[0]);
            return NULL;
        }

        *module = get_destination_module(argv[0], MODULE_NAME_MAX_LEN);
        if (*module == -1) {
            console_printf("Illegal module %s\n", argv[0]);
            return NULL;
        }

        return argv[1];
    }

    *module = default_module;
    return argv[0];
}

static int
show_cmd_help(char *argv[])
{
    const char *command = NULL;
    int module = -1;
    const struct shell_module *shell_module = NULL;
    int i;

    command = get_command_and_module(argv, &module);
    if ((module == -1) || (command == NULL)) {
        return 0;
    }

    shell_module = &shell_modules[module];
    for (i = 0; shell_module->commands[i].sc_cmd; i++) {
        if (!strcmp(command, shell_module->commands[i].sc_cmd)) {
            console_printf("%s:\n", shell_module->commands[i].sc_cmd);

            if (!shell_module->commands[i].help) {
                console_printf("\n");
                return 0;
            }
            if (shell_module->commands[i].help->usage) {
                console_printf("%s\n", shell_module->commands[i].help->usage);
            } else if (shell_module->commands[i].help->summary) {
                console_printf("%s\n", shell_module->commands[i].help->summary);
            } else {
                console_printf("\n");
            }

            return 0;
        }
    }

    console_printf("Unrecognized command: %s\n", argv[0]);
    return 0;
}

static void
print_modules(void)
{
    int module;

    for (module = 0; module < num_of_shell_entities; module++) {
        console_printf("%s\n", shell_modules[module].module_name);
    }
}

static void
print_module_commands(const int module)
{
    const struct shell_module *shell_module = &shell_modules[module];
    int i;

    console_printf("help\n");

    for (i = 0; shell_module->commands[i].sc_cmd; i++) {
        console_printf("%-30s", shell_module->commands[i].sc_cmd);
        if (shell_module->commands[i].help &&
            shell_module->commands[i].help->summary) {
        console_printf("%s", shell_module->commands[i].help->summary);
        }
        console_printf("\n");
    }
}

static int
show_help(int argc, char *argv[])
{
    int module;

    /* help per command */
    if ((argc > 2) || ((default_module != -1) && (argc == 2))) {
        return show_cmd_help(&argv[1]);
    }

    /* help per module */
    if ((argc == 2) || ((default_module != -1) && (argc == 1))) {
        if (default_module == -1) {
            module = get_destination_module(argv[1], MODULE_NAME_MAX_LEN);
            if (module == -1) {
                console_printf("Illegal module %s\n", argv[1]);
                return 0;
            }
        } else {
            module = default_module;
        }

        print_module_commands(module);
    } else { /* help for all entities */
        console_printf("Available modules:\n");
        print_modules();
        console_printf("To select a module, enter 'select <module name>'.\n");
    }

    return 0;
}

static int
set_default_module(const char *name)
{
    int module;

    if (strlen(name) > MODULE_NAME_MAX_LEN) {
        console_printf("Module name %s is too long, default is not changed\n",
                       name);
        return -1;
    }

    module = get_destination_module(name, MODULE_NAME_MAX_LEN);

    if (module == -1) {
        console_printf("Illegal module %s, default is not changed\n", name);
        return -1;
    }

    default_module = module;

    strncpy(default_module_prompt, name, MODULE_NAME_MAX_LEN);
    strcat(default_module_prompt, "> ");

    return 0;
}

static int
select_module(int argc, char *argv[])
{
    if (argc == 1) {
        default_module = -1;
    } else {
        set_default_module(argv[1]);
    }

    return 0;
}

static shell_cmd_func_t
get_cb(int argc, char *argv[])
{
    const char *first_string = argv[0];
    int module = -1;
    const struct shell_module *shell_module;
    const char *command;
    int i;

    if (!first_string || first_string[0] == '\0') {
        console_printf("Illegal parameter\n");
        return NULL;
    }

    if (!strcmp(first_string, "help")) {
        return show_help;
    }

    if (!strcmp(first_string, "select")) {
        return select_module;
    }

    if ((argc == 1) && (default_module == -1)) {
        console_printf("Missing parameter\n");
        return NULL;
    }

    command = get_command_and_module(argv, &module);
    if ((module == -1) || (command == NULL)) {
        return NULL;
    }

    shell_module = &shell_modules[module];
    console_printf("module: %d, command: %s\n", module, command);
    for (i = 0; shell_module->commands[i].sc_cmd; i++) {
        if (!strcmp(command, shell_module->commands[i].sc_cmd)) {
            return shell_module->commands[i].sc_cmd_func;
        }
    }

    return NULL;
}

static void
shell_process_command(char *line)
{
    char *argv[MYNEWT_VAL(SHELL_CMD_ARGC_MAX) + 1];
    shell_cmd_func_t sc_cmd_func;
    size_t argc_offset = 0;
    size_t argc;

    argc = line2argv(line, argv, MYNEWT_VAL(SHELL_CMD_ARGC_MAX) + 1);
    if (!argc) {
        console_printf("%s", get_prompt());
        return;
    }

    sc_cmd_func = get_cb(argc, argv);
    if (!sc_cmd_func) {
        if (app_cmd_handler != NULL) {
            sc_cmd_func = app_cmd_handler;
        } else {
            console_printf("Unrecognized command: %s\n", argv[0]);
            console_printf("Type 'help' for list of available commands\n");
            console_printf("%s", get_prompt());
            return;
        }
    }

    /* Allow invoking a cmd with module name as a prefix; a command should
     * not know how it was invoked (with or without prefix)
     */
    if (default_module == -1 && sc_cmd_func != select_module && sc_cmd_func != show_help) {
        argc_offset = 1;
    }

    /* Execute callback with arguments */
    if (sc_cmd_func(argc - argc_offset, &argv[argc_offset]) < 0) {
        show_cmd_help(argv);
    }

    console_printf("%s", get_prompt());
}

static void
shell(struct os_event *ev)
{
    struct console_input *cmd;

    if (!ev) {
        console_printf("%s", get_prompt());
        return;
    }

    cmd = ev->ev_arg;
    if (!cmd) {
        console_printf("%s", get_prompt());
        return;
    }

    shell_process_command(cmd->line);
    os_eventq_put(&avail_queue, ev);
}

void
shell_register_app_cmd_handler(shell_cmd_func_t handler)
{
    app_cmd_handler = handler;
}

void
shell_register_prompt_handler(shell_prompt_function_t handler)
{
    app_prompt_handler = handler;
}

void
shell_register_default_module(const char *name)
{
    int result = set_default_module(name);

    if (result != -1) {
        console_printf("\n");
        console_printf("%s", default_module_prompt);
    }
}

static void
line_queue_init(void)
{
    int i;

    for (i = 0; i < MYNEWT_VAL(SHELL_MAX_CMD_QUEUED); i++) {
        shell_console_ev[i].ev_cb = shell;
        shell_console_ev[i].ev_arg = &buf[i];
        os_eventq_put(&avail_queue, &shell_console_ev[i]);
    }
}

int
shell_register(const char *module_name, const struct shell_cmd *commands)
{
    if (num_of_shell_entities >= MYNEWT_VAL(SHELL_MAX_MODULES)) {
        return -1;
    }

    shell_modules[num_of_shell_entities].module_name = module_name;
    shell_modules[num_of_shell_entities].commands = commands;
    ++num_of_shell_entities;

    return 0;
}

void
shell_init(void)
{
    /* Ensure this function only gets called by sysinit. */
    SYSINIT_ASSERT_ACTIVE();

#if !MYNEWT_VAL(SHELL_TASK)
    return;
#endif

    os_eventq_init(&avail_queue);
    line_queue_init();
    prompt = SHELL_PROMPT;
    console_set_queues(&avail_queue, os_eventq_dflt_get());
}
