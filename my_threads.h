void my_thr_create(int thr_id, void (*func) (void*), void* arg);
void my_thr_start();
void my_thr_yield();
void my_thr_exit();