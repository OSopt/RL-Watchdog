#include <linux/module.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/syscalls.h>
#include <linux/blkdev.h>
#include <linux/nvme.h>
#include <linux/pci.h>
#include <linux/nvme_ioctl.h>
#include <linux/cdev.h>
#include <linux/blk-mq.h>
#include "nvme.h"

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

MODULE_IMPORT_NS(NVME_TARGET_PASSTHRU);
#define INVALID_OPCODE (0xFF)
#define MAX_DEVICES (32)
#define MAX_PATH_LEN (64)

static char* device_list = "";
static char* parsed_device_list[MAX_DEVICES];
static char validated_device_path[MAX_DEVICES][MAX_PATH_LEN] = {0,};
static int num_devices = 0;
static int cur_idx = 0;
static long polling_duration_ms = 1000;
static long timeout_ms = 0;
static long max_kiops = 480;
module_param(device_list, charp, 0);
module_param(polling_duration_ms, long, 0);
module_param(timeout_ms, long, 0);
module_param(max_kiops, long, 0);

static bool rl_on = false;

void parse_device_list(void) {
  char* ret = strsep(&device_list, ",");
  while(ret) {
    parsed_device_list[num_devices] = ret;
    num_devices++;
    if (num_devices >= MAX_DEVICES) {
      printk("Max %d devices can be processed",  MAX_DEVICES);
      break;
    }
    ret = strsep(&device_list, ",");
  }
}

bool validate_path(char* path) {
  struct file* file = filp_open(path, O_RDONLY, 0);
  if (IS_ERR(file)) {
    return false;
  } else {
    filp_close(file, NULL);
    return true;
  }
}

void validate_device_list(void) {
  int count = 0;
  for (int i = 0 ; i < num_devices ; i++) {
    char target_path[MAX_PATH_LEN] = "/dev/";
    if (strlen(target_path) + strlen(parsed_device_list[i]) >= MAX_PATH_LEN) {
      printk("Too long path %s", parsed_device_list[i]);
      continue;
    }
    strcat(target_path, parsed_device_list[i]);
    {
      strcpy(validated_device_path[count], target_path);
      if (validate_path(validated_device_path[count]) == false) {
        printk("file open error");
        validated_device_path[count][0] = '\0';
      }
      count++;
    }
  }
  num_devices = count;
  for (int i = 0 ; i < num_devices ; i++) {
    printk("%d: %s", i, validated_device_path[i]);
  }
}

void remove_cur_dev(void) {
  validated_device_path[cur_idx][0] = 0;
}

int get_next_nvme_dev(void) {
  if (num_devices > 0) {
    cur_idx = (cur_idx + 1) % num_devices;
  }
  return cur_idx;
}
#define NUM_LAT (4)
#define NUM_IOPS (3)
#define NUM_SIZE (4)
#define NUM_INFLIGHT (2)

const int ALPHA = 3;
const int GAMMA = 8;
const int REWARD_CORRECT = 10;
const int REWARD_SOME_CORRECT = -5;
const int REWARD_NOT_CORRECT = -10;
const int WORST_LAT = 1000; // 1 second
int lat_bound[NUM_LAT - 1] = {1, 4, 16};
int iops_bound[NUM_IOPS - 1];
int size_bound[NUM_SIZE - 1] = {8, 32, 128};
int inflight_bound[NUM_INFLIGHT - 1] = {13};

int q_table[NUM_IOPS][NUM_SIZE][NUM_INFLIGHT][NUM_LAT] = {0,};

int get_idx(int* bound, int num, int val) {
  int i = 0;
  for (; i < num - 1 ; i++) {
    if (bound[i] > val) {
      break;
    }
  }
  return i;
}

int get_inflight_idx(int inflight) {
  return get_idx(inflight_bound, NUM_INFLIGHT, inflight);
}

int get_size_idx(int size) {
  return get_idx(size_bound, NUM_SIZE, size);
}
int get_iops_idx(int iops) {
  return get_idx(iops_bound, NUM_IOPS, iops);
}

int get_lat_idx(int lat) {
  return get_idx(lat_bound, NUM_LAT, lat);
}

unsigned infer_timeout(unsigned long iops, unsigned long inflight,
    unsigned long size, int** max_loc) {
  int inflight_idx = get_inflight_idx(inflight);
  int size_idx = get_size_idx(size);
  int iops_idx = get_iops_idx(iops);
  int max_idx = NUM_LAT - 1;

  *max_loc = &q_table[iops_idx][size_idx][inflight_idx][NUM_LAT - 1];
  for (int i = NUM_LAT - 2; i >= 0 ; i--) {
    if (**max_loc < q_table[iops_idx][size_idx][inflight_idx][i]) {
      *max_loc = &q_table[iops_idx][size_idx][inflight_idx][i];
      max_idx = i;
    }
  }
  return max_idx;
}

const int CALIBRATION = 100000;

void feedback(int* prev_loc, int cur_q, int reward) {
  if (prev_loc != NULL) {
    int val = *prev_loc / CALIBRATION / 10;
    int diff = ALPHA * (reward + GAMMA * cur_q / CALIBRATION / 100 - val) * CALIBRATION;
    if (INT_MAX - val > diff || INT_MIN - val < diff) {
      *prev_loc += diff;
    }
    //    printf("%lf %d %lf %d\n", *prev_loc, val, cur_q, diff);
  }
}

int watchdog_fn(void* arg) {
  int no_device_list = strlen(device_list) == 0;
  struct file* devs[MAX_DEVICES] = {0,};
  struct device* raw_devs[MAX_DEVICES] = {0,};
  struct nvme_command c;
  unsigned long prev_stats[5] = {0,};
  bool first = true;
  int* prev_loc = NULL;
  int prev_reward = 0;
  int spec_max_iops = max_kiops * 1000;
  long good = 0, bad = 0, some_bad = 0, count = 0;
  spec_max_iops /= 16;
  memset(&c, 0x0, sizeof(struct nvme_command));
  c.common.opcode = INVALID_OPCODE;
  parse_device_list();
  validate_device_list();
  for (int i = NUM_IOPS - 2 ; i >= 0 ; i--) {
    iops_bound[i] = spec_max_iops;
    spec_max_iops >>= 4;
  }

  for (int i = 0 ; i < num_devices ; i++) {
    if (validated_device_path[i][0] != '\0') {
      devs[i] = filp_open(validated_device_path[i], O_RDWR, 0);
      {
        struct nvme_ctrl* ctrl = devs[i]->private_data;
        struct nvme_ns* ns = nvme_find_get_ns(ctrl, 1);
        struct gendisk* gendisk = ns->disk;
        raw_devs[i] = disk_to_dev(gendisk);
        nvme_put_ns(ns);
      }
    }
  }
  while(!kthread_should_stop()) {
    if (no_device_list) {
//      printk("no device input");
    } else {
      int idx = get_next_nvme_dev();
      if (devs[idx] == NULL) {
//        printk("no valid nvme device");
      } else {
        int reward = 0;
        int ret = 0;
        u64 result = 0;
        struct file* dev = devs[idx];
        unsigned long stats[5];
        long long time_diff;
        struct nvme_ctrl* ctrl = devs[idx]->private_data;
        ktime_t start_time, end_time;
        unsigned timeout_idx = 0;
        unsigned timeout = 0;
        int real_time_idx = 0;
        unsigned long iops = 0, size = 0, inflight = 0;
        int* loc = NULL;
        part_stat_get2(raw_devs[idx], stats);
        if (rl_on) {
          iops = stats[1] - prev_stats[1];
          inflight = stats[0];
          if (iops == 0) {
            size = 0;
          } else {
            size = (stats[2] - prev_stats[2]) * 512 / iops;
          }
          timeout_idx = infer_timeout(iops, inflight, size, &loc);

          if (timeout_idx < NUM_LAT - 1) {
            timeout = msecs_to_jiffies(lat_bound[timeout_idx]);
          } else {
            timeout = msecs_to_jiffies(WORST_LAT);
          }
          //timeout = msecs_to_jiffies(WORST_LAT);

        }
        else {
          timeout = msecs_to_jiffies(timeout_ms);
        }
        start_time = ktime_get();
        ret = nvme_submit_user_cmd(ctrl->admin_q, &c, NULL, 0, NULL, 0, 0, &result, timeout, false);
				end_time = ktime_get();
				time_diff = ktime_to_ns(ktime_sub(end_time, start_time));
        if (rl_on) {
          real_time_idx = get_lat_idx(time_diff/1000000);
        }
    //    printk("inferred expected_idx %d exeuted_idx %d", timeout_idx, real_time_idx);
#if 0
        if (first) {
        } else {
          printk("inflights %lu w_ios %lu w_secs %lu r_ios %lu r_secs %lu duration %lld ns", stats[0], stats[1] - prev_stats[1], stats[2] - prev_stats[2], stats[3] - prev_stats[3], stats[4] - prev_stats[4], time_diff);
        }
#endif
        if (ret == -4) {
          msleep(4000);
          if (validate_path(validated_device_path[idx])) {
            if (rl_on) {
              reward = REWARD_NOT_CORRECT;
              bad++;
            }
            printk("inference failed");
          } else {
            filp_close(dev, NULL);
            devs[idx] = NULL;
            printk("device failure detected");
          }
        }
        else {
          if (rl_on) {
            if (timeout_idx == real_time_idx) {
              reward = REWARD_CORRECT;
              good++;
            } else if (timeout_idx < real_time_idx) {
              reward = REWARD_NOT_CORRECT;
              bad++;
            } else {
              reward = REWARD_SOME_CORRECT;
              some_bad++;
            }
          }
        }
				if (rl_on) {
					feedback(prev_loc, *loc, prev_reward);
				}
				count++;
				prev_stats[0] = stats[0];
				prev_stats[1] = stats[1];
				prev_stats[2] = stats[2];
				prev_stats[3] = stats[3];
				prev_stats[4] = stats[4];
				prev_loc = loc;
				prev_reward = reward;

				first = false;
				if (count % 1000 == 0) {
					//          printk("%ld: %ld %ld %ld ratio %ld", count, good, some_bad, bad, good * 100 / count);
				}
			}
    }
    msleep(polling_duration_ms);
  }

  for (int i = 0 ; i < num_devices; i++) {
    if (devs[i]) {
      filp_close(devs[i], NULL);
    }
  }

  return 0;
}

static struct task_struct* watchdog_task = NULL;

static int watchdog_mod_init(void) {

  printk("%s\n", __func__);
  if (timeout_ms == 0) {
    rl_on = true;
  }
  printk("arguments: device_list %s polling_duration_ms %ld timeout_ms %ld max_kiops %ld rl_on %d", device_list, polling_duration_ms, timeout_ms, max_kiops, rl_on);

  watchdog_task = kthread_run(watchdog_fn, NULL, "pcie_watchdog");
  if (watchdog_task) {
    printk("watchdog added\n");
  } else {
    printk("watchdog failed\n");
  }
  return 0;

}

static void watchdog_mod_exit(void) {
  printk("%s\n", __func__);
  if (watchdog_task) {
    kthread_stop(watchdog_task);
  }
}

module_init(watchdog_mod_init);
module_exit(watchdog_mod_exit);
