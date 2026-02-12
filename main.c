#include<stdio.h>
#include"core/monitor/cpu_utilize.h"
#include<stdlib.h>
#include<unistd.h>

int main(){
    printf("hello from main\n");
    int res=monitor_cpu_util_init();
    if(res==-1){
        return 1;
    }
    while(1){
        double res=monitor_cpu_util();
        if(res==1.0){
            return 1;
        }
        printf("%lf percentage of cpu is being utilised\n",res);
        sleep(1);
    }
    return 0;
}