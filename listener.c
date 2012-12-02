/*
 * Copyright 2012 Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "watchman.h"
#include <fnmatch.h>

static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
static w_ht_t *clients = NULL;
static w_ht_t *command_funcs = NULL;

static json_t *make_response(void)
{
  json_t *resp = json_object();

  json_object_set_new(resp, "version", json_string(PACKAGE_VERSION));

  return resp;
}

static int client_json_write(const char *buffer, size_t size, void *ptr)
{
  struct watchman_client *client = ptr;
  int res;

  res = write(client->fd, buffer, size);

  return (size_t)res == size ? 0 : -1;
}

static void send_and_dispose_response(struct watchman_client *client,
    json_t *response)
{
  json_dump_callback(response, client_json_write, client, JSON_COMPACT);
  write(client->fd, "\n", 1);
  json_decref(response);
}

static void send_error_response(struct watchman_client *client,
    const char *fmt, ...)
{
  char buf[WATCHMAN_NAME_MAX];
  va_list ap;
  json_t *resp = make_response();

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  json_object_set_new(resp, "error", json_string(buf));

  send_and_dispose_response(client, resp);
}

static void client_delete(w_ht_val_t val)
{
  struct watchman_client *client = (struct watchman_client*)val;

  close(client->fd);
  free(client);
}

static struct watchman_hash_funcs client_hash_funcs = {
  NULL, // copy_key
  NULL, // del_key
  NULL, // equal_key
  NULL, // hash_key
  NULL, // copy_val
  client_delete
};

/* Parses filename match rules.
 * By default, we want to include items that positively match
 * the set of fnmatch(3) patterns specified.
 * If -X is specified, we switch to exclude mode; any patterns
 * that are encountered after -X are excluded from the result set.
 * If -I is specified, we switch to include mode, so you can use
 * -I to turn on include mode again after using -X.
 * If "!" is specified, the following pattern is negated.
 * We switch back out of negation mode after that pattern.
 *
 * We stop processing args when we find "--" and update
 * *next_arg to the argv index after that argument.
 */
static bool parse_watch_params(int start, json_t *args,
    struct watchman_rule **head_ptr,
    uint32_t *next_arg)
{
  bool include = true;
  bool negated = false;
  struct watchman_rule *rule, *prior = NULL;
  uint32_t i;

  if (!json_is_array(args)) {
    return false;
  }
  *head_ptr = NULL;

  for (i = start; i < json_array_size(args); i++) {
    const char *arg = json_string_value(json_array_get(args, i));
    if (!arg) {
      /* not a string value! */
      return false; // FIXME: leak
    }

    if (!strcmp(arg, "--")) {
      i++;
      break;
    }
    if (!strcmp(arg, "-X")) {
      include = false;
      continue;
    }
    if (!strcmp(arg, "-I")) {
      include = true;
      continue;
    }
    if (!strcmp(arg, "!")) {
      negated = true;
      continue;
    }

    rule = calloc(1, sizeof(*rule));
    if (!rule) {
      return false; // FIXME: leak
    }

    rule->include = include;
    rule->negated = negated;
    // We default the fnmatch so that we can match against paths that include
    // slashes.
    // To recursively match the contents of a dir, use "dir/*".  To match all
    // "C" source files, use "*.c".  To match all makefiles, use
    // "*/Makefile" + "Makefile" (include the latter if the Makefile might
    // be at the top level).
    rule->pattern = strdup(arg);
    rule->flags = FNM_PERIOD;

    if (!prior) {
      *head_ptr = rule;
    } else {
      prior->next = rule;
    }
    prior = rule;

    printf("made rule %s %s %s\n",
        rule->include ? "-I" : "-X",
        rule->negated ? "!" : "",
        rule->pattern);

    // Reset negated flag
    negated = false;
  }

  if (next_arg) {
    *next_arg = i;
  }

  return true;
}

// must be called with root locked
uint32_t w_rules_match(w_root_t *root,
    struct watchman_file *oldest_file,
    w_ht_t *uniq, struct watchman_rule *head)
{
  struct watchman_file *file;
  struct watchman_rule *rule;
  w_string_t *full_name;
  w_string_t *relname;
  uint32_t num_matches = 0;
  uint32_t name_start;

  name_start = root->root_path->len + 1;

  for (file = oldest_file; file; file = file->prev) {
    // no rules means return everything
    bool matched = (head == NULL) ? true : false;

    full_name = w_string_path_cat(file->parent->path, file->name);
    // Record the name relative to the root
    relname = w_string_slice(full_name, name_start,
                full_name->len - name_start);
    w_string_delref(full_name);

    // Work through the rules; we stop as soon as we get a match.
    for (rule = head; rule && !matched; rule = rule->next) {

      // In theory, relname->buf may not be NUL terminated in
      // the right spot if it was created as a slice.
      // In practice, we don't see those, but if we do, we should
      // probably make a copy of the string into a stack buffer :-/
      matched = fnmatch(rule->pattern, relname->buf, rule->flags) == 0;

      // If the rule is negated, we negate the sense of the
      // match result
      if (rule->negated) {
        matched = !matched;
      }

      // If the pattern matched then we're going to include the file
      // in our result set, but only if it is set to include.
      // If we're not including, we explicitly don't want to know
      // about the file, so pretend it didn't match and stop processing
      // rules for the file.
      if (matched && !rule->include) {
        matched = false;
        break;
      }
    }

    if (matched) {

      w_ht_set(uniq, (w_ht_val_t)relname, (w_ht_val_t)file);
      num_matches++;
    }

    w_string_delref(relname);
  }

  return num_matches;
}

typedef void (*watchman_command_func)(
    struct watchman_client *client,
    json_t *args);

static void run_rules(struct watchman_client *client,
    w_root_t *root,
    w_clock_t *since,
    struct watchman_rule *rules)
{
  w_ht_iter_t iter;
  uint32_t matches;
  w_ht_t *uniq;
  struct watchman_file *oldest = NULL, *f;
  json_t *response = make_response();
  json_t *file_list = json_array();

  uniq = w_ht_new(8, &w_ht_string_funcs);
  printf("running rules!\n");

  w_root_lock(root);
  for (f = root->latest_file; f; f = f->next) {
    if (since && f->otime.seconds < since->seconds) {
      break;
    }
    oldest = f;
  }
  matches = w_rules_match(root, oldest, uniq, rules);
  w_root_unlock(root);

  printf("rules were run, we have %" PRIu32 " matches\n", matches);

  if (w_ht_first(uniq, &iter)) do {
    struct watchman_file *file = (struct watchman_file*)iter.value;
    w_string_t *relname = (w_string_t*)iter.key;
    json_t *record = json_object();

    json_object_set_new(record, "name", json_string(relname->buf));
    json_object_set_new(record, "exists", json_boolean(file->exists));

    json_array_append_new(file_list, record);

  } while (w_ht_next(uniq, &iter));

  w_ht_free(uniq);

  json_object_set_new(response, "files", file_list);

  send_and_dispose_response(client, response);
}

static w_root_t *resolve_root_or_err(
    struct watchman_client *client,
    json_t *args,
    int root_index,
    bool create)
{
  w_root_t *root;
  const char *root_name;
  json_t *ele;

  ele = json_array_get(args, root_index);
  if (!ele) {
    send_error_response(client, "wrong number of arguments");
    return NULL;
  }

  root_name = json_string_value(ele);
  if (!root_name) {
    send_error_response(client,
        "invalid value for argument %d, expected "
        "a string naming the root dir",
        root_index);
    return NULL;
  }

  root = w_root_resolve(root_name, create);
  if (!root) {
    send_error_response(client,
        "unable to resolve root %s",
        root_name);
  }
  return root;
}

/* find /root [patterns] */
static void cmd_find(
    struct watchman_client *client,
    json_t *args)
{
  struct watchman_rule *rules = NULL;
  w_root_t *root;

  /* resolve the root */
  if (json_array_size(args) < 2) {
    send_error_response(client, "not enough arguments for 'find'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  /* parse argv into a chain of watchman_rule */
  if (!parse_watch_params(2, args, &rules, NULL)) {
    send_error_response(client, "invalid rule spec: %s", strerror(errno));
    return;
  }

  /* now find all matching files */
  run_rules(client, root, NULL, rules);
}

/* since /root <timestamp> [patterns] */
static void cmd_since(
    struct watchman_client *client,
    json_t *args)
{
  struct watchman_rule *rules = NULL;
  w_root_t *root;
  w_clock_t since;
  json_t *clock_ele;

  /* resolve the root */
  if (json_array_size(args) < 3) {
    send_error_response(client, "not enough arguments for 'since'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, false);
  if (!root) {
    return;
  }

  // FIXME: allow using a safer clock representation instead.
  clock_ele = json_array_get(args, 2);
  if (json_is_integer(clock_ele)) {
    since.seconds = json_integer_value(clock_ele);
  } else if (json_is_string(clock_ele)) {
    since.seconds = atoi(json_string_value(clock_ele));
  } else {
    send_error_response(client,
        "expected argument 2 to be a valid clock/timespec");
    return;
  }

  /* parse argv into a chain of watchman_rule */
  if (!parse_watch_params(3, args, &rules, NULL)) {
    send_error_response(client, "invalid rule spec: %s", strerror(errno));
    return;
  }

  /* now find all matching files */
  run_rules(client, root, &since, rules);
}

/* trigger /root [watch patterns] -- cmd to run
 * Sets up a trigger so that we can execute a command when a change
 * is detected */
static void cmd_trigger(
    struct watchman_client *client,
    json_t *args)
{
  struct watchman_rule *rules;
  w_root_t *root;
  uint32_t next_arg = 0;
  struct watchman_trigger_command *cmd;
  json_t *resp;

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  if (!parse_watch_params(2, args, &rules, &next_arg)) {
    send_error_response(client, "invalid rule spec: %s", strerror(errno));
    return;
  }

  if (next_arg >= json_array_size(args)) {
    send_error_response(client, "no command was specified");
    return;
  }

  cmd = calloc(1, sizeof(*cmd));
  if (!cmd) {
    send_error_response(client, "no memory!");
    return;
  }

  cmd->rules = rules;
  cmd->argc = json_array_size(args) - next_arg;
  cmd->argv = w_argv_copy_from_json(args, next_arg);
  if (!cmd->argv) {
    free(cmd);
    send_error_response(client, "unable to build argv array");
    return;
  }

  w_root_lock(root);
  cmd->triggerid = ++root->next_cmd_id;
  w_ht_set(root->commands, cmd->triggerid, (w_ht_val_t)cmd);
  w_root_unlock(root);

  resp = make_response();
  json_object_set_new(resp, "triggerid", json_integer(cmd->triggerid));
  send_and_dispose_response(client, resp);
}

/* watch /root */
static void cmd_watch(
    struct watchman_client *client,
    json_t *args)
{
  w_root_t *root;
  json_t *resp;

  /* resolve the root */
  if (json_array_size(args) != 2) {
    send_error_response(client, "wrong number of arguments to 'watch'");
    return;
  }

  root = resolve_root_or_err(client, args, 1, true);
  if (!root) {
    return;
  }

  resp = make_response();
  json_object_set_new(resp, "watch", json_string(root->root_path->buf));
  send_and_dispose_response(client, resp);
}

static void cmd_shutdown(
    struct watchman_client *client,
    json_t *args)
{
  (void)client;
  (void)args;
  exit(0);
}

static void cmd_shutdown(
    struct watchman_client *client,
    int argc,
    char **argv)
{
  (void)client;
  (void)argc;
  (void)argv;
  exit(0);
}

static struct {
  const char *name;
  watchman_command_func func;
} commands[] = {
  { "find", cmd_find },
  { "since", cmd_since },
  { "watch", cmd_watch },
  { "trigger", cmd_trigger },
  { "shutdown-server", cmd_shutdown },
  { NULL, NULL }
};


static size_t client_read_json(void *buffer, size_t buflen, void *ptr)
{
  struct watchman_client *client = ptr;

  return read(client->fd, buffer, buflen);
}

// The client thread reads and decodes json packets,
// then dispatches the commands that it finds
// TODO: want to allow notifications to be sent over
// the socket as we notice them
static void *client_thread(void *ptr)
{
  struct watchman_client *client = ptr;
  int i;
  watchman_command_func func;
  const char *cmd_name;
  w_string_t *cmd;
  json_t *request;
  json_error_t jerr;

  while (true) {
    memset(&jerr, 0, sizeof(jerr));
    request = json_load_callback(client_read_json, client,
        JSON_DISABLE_EOF_CHECK, &jerr);

    if (!request) {
      send_error_response(client, "invalid json at position %d: %s",
          jerr.position, jerr.text);

      pthread_mutex_lock(&client_lock);
      w_ht_del(clients, client->fd);
      pthread_mutex_unlock(&client_lock);
      break;
    }

    if (!json_array_size(request)) {
      send_error_response(client,
          "invalid command (expected an array with some elements!)");
      continue;
    }

    cmd_name = json_string_value(json_array_get(request, 0));
    if (!cmd_name) {
      send_error_response(client,
          "invalid command: expected element 0 to be the command name");
      continue;
    }
    cmd = w_string_new(cmd_name);
    func = (watchman_command_func)w_ht_get(command_funcs, (w_ht_val_t)cmd);
    w_string_delref(cmd);

    if (func) {
      func(client, request);
    } else {
      send_error_response(client, "unknown command %s", cmd_name);
    }

    json_decref(request);
  }

  return NULL;
}

bool w_start_listener(const char *path)
{
  int fd;
  int i;
  struct sockaddr_un un;

  if (strlen(path) >= sizeof(un.sun_path) - 1) {
    fprintf(stderr, "%s: path is too long\n",
        path);
    return false;
  }

  signal(SIGPIPE, SIG_IGN);

  fd = socket(PF_LOCAL, SOCK_STREAM, 0);
  if (fd == -1) {
    perror("socket");
    return false;
  }

  un.sun_family = PF_LOCAL;
  strcpy(un.sun_path, path);

  if (bind(fd, (struct sockaddr*)&un, sizeof(un)) != 0) {
    fprintf(stderr, "bind(%s): %s\n",
      path, strerror(errno));
    close(fd);
    return false;
  }

  if (listen(fd, 200) != 0) {
    fprintf(stderr, "listen(%s): %s\n",
        path, strerror(errno));
    close(fd);
    return false;
  }

  w_set_cloexec(fd);

  if (!clients) {
    clients = w_ht_new(2, &client_hash_funcs);
  }

  // Wire up the command handlers
  command_funcs = w_ht_new(16, &w_ht_string_funcs);
  for (i = 0; commands[i].name; i++) {
    w_ht_set(command_funcs,
        (w_ht_val_t)w_string_new(commands[i].name),
        (w_ht_val_t)commands[i].func);
  }

  // Now run the dispatch
  while (true) {
    int client_fd;
    struct watchman_client *client;
    pthread_t thr;
    pthread_attr_t attr;

    client_fd = accept(fd, NULL, 0);
    if (client_fd == -1) {
      continue;
    }
    w_set_cloexec(client_fd);

    client = calloc(1, sizeof(*client));
    client->fd = client_fd;

    pthread_mutex_lock(&client_lock);
    w_ht_set(clients, client->fd, (w_ht_val_t)client);
    pthread_mutex_unlock(&client_lock);

    // Start a thread for the client.
    // We used to use libevent for this, but we have
    // a low volume of concurrent clients and the json
    // parse/encode APIs are not easily used in a non-blocking
    // server architecture.
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thr, &attr, client_thread, client)) {
      // It didn't work out, sorry!
      pthread_mutex_lock(&client_lock);
      w_ht_del(clients, client->fd);
      pthread_mutex_unlock(&client_lock);
    }
    pthread_attr_destroy(&attr);
  }

  return true;
}

/* vim:ts=2:sw=2:et:
 */
