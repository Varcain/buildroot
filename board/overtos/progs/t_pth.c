#include <stdio.h>
#include <pthread.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static int counter;
static void *worker(void *a){ long id=(long)a; for(int i=0;i<1000;i++){pthread_mutex_lock(&m);counter++;pthread_mutex_unlock(&m);} printf("thread %ld done\n",id); return (void*)(id*10); }
int main(void){
  pthread_t t1,t2; void *r1,*r2;
  pthread_create(&t1,NULL,worker,(void*)1);
  pthread_create(&t2,NULL,worker,(void*)2);
  pthread_join(t1,&r1); pthread_join(t2,&r2);
  printf("joined counter=%d r1=%ld r2=%ld\n",counter,(long)r1,(long)r2);
  return 0;
}
