#ifndef _VARIANT_BULLHEAD_H_
#define _VARIANT_BULLHEAD_H_

#ifdef __cplusplus
extern "C" {
#endif

//we have LSE in bullhead
#define HAVE_LSE                    true

//spi bus for comms
#define PLATFORM_HOST_INTF_SPI_BUS  0

#define AP_INT_WAKEUP               GPIO_PA(3)
#undef AP_INT_NONWAKEUP

#ifdef __cplusplus
}
#endif

#endif
