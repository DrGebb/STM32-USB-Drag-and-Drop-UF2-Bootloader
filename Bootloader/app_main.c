#include "app_main.h"
#include "cmsis_os2.h"
#include <string.h>




/********** TASKS *****************/
static void usb_thread (void *arg) {
  (void)arg;

  if (USBD_Initialize(0U) != usbOK) {   
    for (;;) { osDelay(1000); }         
  }

  if (USBD_Connect(0U) != usbOK) {
    for (;;) { osDelay(1000); }
  }

  for (;;) {
   
    osDelay(100);
  }
}

static const osThreadAttr_t usb_attr = {
  .name       = "usb_init",
  .stack_size = 2048U
};



void app_main() {

  NVIC_SetPriorityGrouping (3); 
  osKernelInitialize();
  osThreadNew(usb_thread, NULL, &usb_attr);  
  osThreadNew(boot_supervisor_thread, NULL, NULL);
  osKernelStart();
    
  while (1) {
    
  } 
        
} 
  
 