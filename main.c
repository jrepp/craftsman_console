#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "connection.c"
#include "dlfn.h"
#include "execute.c"
#include "float.h"
#include "functor.h"
#include "hash.h"
#include "input.h"
#include "loop.c"
#include "macro.h"
#include "notify.h"
#include "record.h"
#include "stats.h"

static const char *dlpath = "code/feature.so";
static const char *watch_dirs[] = { "code" };
static uint32_t simulation_goal;
static bool exiting;
static RecordRW_t input_rw;

typedef struct {
  uint32_t turn_command_count;
  char *command[MAX_PLAYER];
  size_t command_len[MAX_PLAYER];
} CommandFrame_t;

typedef struct {
  uint32_t turn_nearest;
  uint32_t turn_farthest;
} CommandPreview_t;

void
command_preview(size_t player_count, RecordRW_t recording[player_count],
                CommandPreview_t *out_state)
{
  CommandPreview_t ret_state = { .turn_nearest = loop_input_queue_max(),
                                 .turn_farthest = 0 };
  for (int i = 0; i < player_count; ++i) {
    uint32_t rcmd = recording[i].read.command_count;
    uint32_t wcmd = recording[i].write.command_count;

    ret_state.turn_farthest = MAX(wcmd - rcmd, ret_state.turn_farthest);
    ret_state.turn_nearest = MIN(wcmd - rcmd, ret_state.turn_nearest);
  }

  *out_state = ret_state;
}

#define FRAME_BUF_LEN 4096
bool
command_frame(size_t player_count, RecordRW_t recording[player_count],
              CommandFrame_t *out_state)
{
  static char buffer[FRAME_BUF_LEN];
  CommandFrame_t ret_state = { .turn_command_count = player_count };
  char *buffer_write = buffer;
  char *buffer_end = buffer + FRAME_BUF_LEN;

  for (int i = 0; i < player_count; ++i) {
    size_t cmd_len = 0;
    const char *cmd =
      record_read(recording[i].rec, &recording[i].read, &cmd_len);
    if (buffer_write + cmd_len >= buffer_end)
      return false;
    memcpy(buffer_write, cmd, cmd_len);
    buffer_write[cmd_len] = 0;

    ret_state.command[i] = buffer_write;
    ret_state.command_len[i] = cmd_len;
    buffer_write += cmd_len + 1;
  }

  *out_state = ret_state;

  return true;
}

uint32_t
game_players(RecordRW_t recording[static MAX_PLAYER])
{
  uint32_t count = 0;
  for (int i = 0; i < MAX_PLAYER; ++i) {
    count += (recording[i].rec != 0);
  }

  return count;
}

void
input_callback(size_t len, char *input)
{
  // These events are not recorded
  switch (input[0]) {
  case 'r':
    simulation_goal = 0;
    loop_halt();
    return;
  case 'q':
    simulation_goal = 0;
    exiting = true;
    loop_halt();
    return;
  case 'i':
    loop_print_status();
    return;
  }

  record_append(input_rw.rec, len, input, &input_rw.write);
}

void
execute_any(size_t len, char *input)
{
  switch (input[0]) {
  case 'b':
    execute_benchmark();
    return;
  case 's':
    simulation_goal = execute_simulation(len, input);
    return;
  case 'a':
    execute_apply(len, input);
    return;
  case 'o':
    execute_object(len, input);
    return;
  case 'h':
    execute_hash(len, input);
    return;
  }
}

void
prompt(int player_count)
{
  dlfn_print_symbols();
  dlfn_print_objects();
  printf("Simulation will run until frame %d.\n", simulation_goal);
  printf("Player Count: %d\n", player_count);
  puts("(q)uit (i)nfo (s)imulation (b)enchmark (a)pply (h)ash "
       "(o)bject "
       "(r)eload>");
}

void
notify_callback(int idx, const struct inotify_event *event)
{
  printf("File change %s\n", event->name);
  if (!strstr(dlpath, event->name))
    return;

  simulation_goal = 0;
  loop_halt();
}

void
game_simulation(RecordRW_t game_record[static MAX_PLAYER])
{
  const int player_count = game_players(game_record);
  Stats_t perfStats[MAX_SYMBOLS];
  double perf[MAX_SYMBOLS];

  stats_init_array(MAX_SYMBOLS, perfStats);

  memset(result, 0, sizeof(result));
  memset(apply_func, 0, sizeof(apply_func));
  used_apply_func = 0;
  memset(result_func, 0, sizeof(result_func));
  used_result_func = 0;

  loop_init(10);
  loop_print_status();
  notify_init(IN_CLOSE_WRITE, ARRAY_LENGTH(watch_dirs), watch_dirs);
  dlfn_init(dlpath);
  dlfn_open();
  simulation_goal = 0;
  input_init();
  prompt(player_count);
  while (loop_run()) {
    // loop_print_frame();

    notify_poll(notify_callback);
    input_poll(input_callback);

    const int target = loop_write_frame();
    while (target > input_rw.write.command_count) {
      record_append(input_rw.rec, 0, 0, &input_rw.write);
    }

    int net_status = connection_sync(target, &input_rw, game_record);
    switch (net_status) {
    case CONN_TERM:
      loop_halt();
      exiting = true;
      continue;
    case CONN_CHANGE:
      loop_halt();
      continue;
    };

    CommandPreview_t preview;
    command_preview(player_count, game_record, &preview);
    if (!preview.turn_nearest) {
      printf("stall [ %d farthest ] [ %d nearest ] \n", preview.turn_farthest,
             preview.turn_nearest);
      input_queue = MIN(input_queue + 1, input_queue_max);
      loop_stall();
      continue;
    }

    if (!loop_fast_forward(preview.turn_farthest)) {
      if (preview.turn_nearest > 1) {
        input_queue = MAX((signed) input_queue - 1, 0);
      }
    }

    CommandFrame_t frame;
    if (!command_frame(player_count, game_record, &frame))
      CRASH();

    for (int i = 0; i < frame.turn_command_count; ++i) {
      execute_any(frame.command_len[i], frame.command[i]);
    }

    if (simulation_goal <= loop_frame()) {
      loop_pause();
      continue;
    }

    for (int i = 0; i < used_apply_func; ++i) {
      functor_invoke(apply_func[i]);
    }

    for (int i = 0; i < dlfn_used_symbols; ++i) {
      uint64_t startCall = rdtsc();
      result[i] = functor_invoke(dlfn_symbols[i].fnctor);
      uint64_t endCall = rdtsc();
      perf[i] = to_double(endCall - startCall);

      for (int j = 0; j < used_result_func; ++j) {
        functor_invoke(result_func[j]);
      }
    }

    for (int i = 0; i < dlfn_used_symbols; ++i) {
      stats_sample_add(&perfStats[i], perf[i]);
    }

    loop_sync();
  }
  puts("--simulation performance");
  for (int i = 0; i < dlfn_used_symbols; ++i) {
    printf("%-20s\t(%5.2e, %5.2e) range\t%5.2e mean ± %4.02f%%\t\n",
           dlfn_symbols[i].name, stats_min(&perfStats[i]),
           stats_max(&perfStats[i]), stats_mean(&perfStats[i]),
           100.0 * stats_rs_dev(&perfStats[i]));
  }
  puts("");
  loop_print_status();
  loop_shutdown();
  notify_shutdown();
  dlfn_shutdown();
  input_shutdown();
}

int
main(int argc, char **argv)
{
  RecordRW_t game_record[MAX_PLAYER] = { 0 };
  const char *host = { 0 };

  bool multiplayer = argc > 1;
  printf("Multiplayer state %d\n", multiplayer);
  if (multiplayer) {
    host = "gamehost.rufe.org";
  }
  if (!connection_init(host)) {
    return 1;
  }
  input_rw.rec = record_alloc();

  while (!exiting) {
    game_simulation(game_record);
    for (int i = 0; i < MAX_PLAYER; ++i) {
      game_record[i].read = (RecordOffset_t){ 0 };
    }
  }

  record_free(input_rw.rec);
  input_rw.rec = NULL;
  for (int i = 0; i < MAX_PLAYER; ++i) {
    record_free(game_record[i].rec);
  }

  connection_term();
  if (multiplayer) {
    connection_print_stats();
  }

  return 0;
}
