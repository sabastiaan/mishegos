#include "mish_common.h"
#include "worker.h"

static volatile bool exiting;
static uint32_t workerno;
static char *worker_name;
static void (*worker_ctor)();
static void (*worker_dtor)();
static try_decode_t try_decode;
static sem_t *mishegos_isems[MISHEGOS_IN_NSLOTS];
static sem_t *mishegos_osem;
static uint8_t *mishegos_arena;

static void init_sems();
static void init_shm();
static void cleanup();
static void exit_sig(int signo);
static void work();

int main(int argc, char const *argv[]) {
  if (argc != 3) {
    errx(1, "Usage: worker <no> <so>");
  }

  DLOG("new worker started: pid=%d", getpid());

  workerno = atoi(argv[1]);
  if (workerno > MISHEGOS_MAX_NWORKERS) {
    errx(1, "workerno > %d", MISHEGOS_MAX_NWORKERS);
  }

  void *so = dlopen(argv[2], RTLD_LAZY);
  if (so == NULL) {
    errx(1, "dlopen: %s", dlerror());
  }

  worker_ctor = dlsym(so, "worker_ctor");
  worker_dtor = dlsym(so, "worker_dtor");

  try_decode = (try_decode_t)dlsym(so, "try_decode");
  if (try_decode == NULL) {
    errx(1, "dlsym: %s", dlerror());
  }

  worker_name = *((char **)dlsym(so, "worker_name"));
  if (worker_name == NULL) {
    errx(1, "dlsym: %s", dlerror());
  }

  DLOG("worker loaded: name=%s", worker_name);

  if (worker_ctor != NULL) {
    worker_ctor();
  }

  init_sems();
  init_shm();

  atexit(cleanup);
  signal(SIGINT, exit_sig);
  signal(SIGTERM, exit_sig);
  signal(SIGABRT, exit_sig);

  work();

  return 0;
}

static void init_sems() {
  for (int i = 0; i < MISHEGOS_IN_NSLOTS; ++i) {
    char sem_name[NAME_MAX + 1] = {};
    snprintf(sem_name, sizeof(sem_name), MISHEGOS_IN_SEMFMT, i);

    mishegos_isems[i] = sem_open(sem_name, O_RDWR, 0644, 1);
    if (mishegos_isems[i] == SEM_FAILED) {
      err(errno, "sem_open: %s", sem_name);
    }

    DLOG("mishegos_isems[%d]=%p", i, mishegos_isems[i]);
  }

  mishegos_osem = sem_open(MISHEGOS_OUT_SEMNAME, O_RDWR, 0644, 1);
  if (mishegos_osem == SEM_FAILED) {
    err(errno, "sem_open: %s", MISHEGOS_OUT_SEMNAME);
  }

  DLOG("mishegos_osem=%p", mishegos_osem);
}

static void init_shm() {
  int fd = shm_open(MISHEGOS_SHMNAME, O_RDWR, 0644);
  if (fd < 0) {
    err(errno, "shm_open: %s", MISHEGOS_SHMNAME);
  }

  mishegos_arena = mmap(NULL, MISHEGOS_SHMSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mishegos_arena == MAP_FAILED) {
    err(errno, "mmap: %s (%ld)", MISHEGOS_SHMNAME, MISHEGOS_SHMSIZE);
  }

  if (close(fd) < 0) {
    err(errno, "close: %s", MISHEGOS_SHMNAME);
  }

  DLOG("mishegos_arena=%p (len=%ld)", mishegos_arena, MISHEGOS_SHMSIZE);
}

static void cleanup() {
  DLOG("cleaning up");

  if (worker_dtor != NULL) {
    worker_dtor();
  }

  munmap(mishegos_arena, MISHEGOS_SHMSIZE);

  for (int i = 0; i < MISHEGOS_IN_NSLOTS; ++i) {
    char sem_name[NAME_MAX + 1] = {};
    snprintf(sem_name, sizeof(sem_name), MISHEGOS_IN_SEMFMT, i);
    sem_close(mishegos_isems[i]);
  }

  sem_close(mishegos_osem);
}

static void exit_sig(int signo) {
  exiting = true;
}

static input_slot *get_first_new_input_slot() {
  input_slot *dest = NULL;

  for (int i = 0; i < MISHEGOS_IN_NSLOTS && dest == NULL; ++i) {
    sem_wait(mishegos_isems[i]);

    input_slot *slot = GET_I_SLOT(i);
    if (!(slot->workers & (1 << workerno))) {
      /* If our bit in the worker mask is low, then we've already
       * processed this slot.
       */
      DLOG("input slot=%d already processed", i);
      goto done;
    }

    dest = malloc(sizeof(input_slot));
    memcpy(dest, slot, sizeof(input_slot));
    slot->workers = slot->workers ^ (1 << workerno);

  done:
    sem_post(mishegos_isems[i]);
  }

  return dest;
}

static void put_first_available_output_slot(output_slot *slot) {
  bool available = false;

  while (!available && !exiting) {
    sleep(1);
    sem_wait(mishegos_osem);

    output_slot *dest = GET_O_SLOT(0);
    if (dest->status != S_NONE) {
      DLOG("output slot=%d occupied", 0);
      goto done;
    }

    memcpy(dest, slot, sizeof(output_slot));
    available = true;

  done:
    sem_post(mishegos_osem);
  }
}

static void work() {
  while (!exiting) {
    DLOG("%s working...", worker_name);

    sleep(1);
    input_slot *input = get_first_new_input_slot();

    if (input != NULL) {
      output_slot *output = try_decode(input->raw_insn, input->len);

      /* Copy our input slot into our output slot, so that we can identify
       * individual runs.
       */
      memcpy(&output->input, input, sizeof(input_slot));
      free(input);

      put_first_available_output_slot(output);
      free(output);
    }
  }

  DLOG("exiting...");
}
