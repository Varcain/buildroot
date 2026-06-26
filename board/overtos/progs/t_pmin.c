#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
static void *worker(void *a){ write(1,"[worker] ran\n",13); return (void*)42; }
int main(void){
  pthread_t t; void *r=0;
  write(1,"[main] create\n",14);
  int rc = pthread_create(&t, NULL, worker, NULL);
  write(1,"[main] created\n",15);
  pthread_join(t, &r);
  char b[32]; int n=0; b[n++]='J'; b[n++]='='; b[n++]='0'+((long)r/10); b[n++]='0'+((long)r%10); b[n++]='\n';
  write(1,b,n);
  return 0;
}
