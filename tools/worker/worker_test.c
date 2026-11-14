#include <stdio.h>
#include <unistd.h>
#include <string.h>

typedef struct {
    char *entryParams;
    struct { void *head; } fdList;
} NCPArgs;

void Main(NCPArgs args) {
    const char *p = "/data/storage/el2/base/haps/entry/files/worker_ok.txt";
    FILE *f = fopen(p, "w");
    if (f) {
        fprintf(f, "worker pid=%d params=%s\n", (int)getpid(), args.entryParams ? args.entryParams : "");
        fclose(f);
    }
    sleep(2);
}
