/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef UNIWILL_ITE8291_H
#define UNIWILL_ITE8291_H

#include <linux/init.h>

int __init uniwill_ite8291_register_driver(void);
void __exit uniwill_ite8291_unregister_driver(void);
int uniwill_ite8291_handle_brightness_event(unsigned long event);

#endif /* UNIWILL_ITE8291_H */
