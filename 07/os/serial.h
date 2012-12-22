#ifndef _SERIAL_H_INCLUDE_
#define _SERIAL_H_INCLUDE_

int serial_init(int index); /*initiate device*/
int serial_is_send_enable(int index); /*Can it enable send?*/
int serial_send_byte(int index, unsigned char b); /*send one word*/

int serial_is_recv_enable(int index);
unsigned char serial_recv_byte(int index);

int serial_intr_is_send_enable(int index);
void serial_intr_send_enable(int index);
void serial_intr_send_disable(int index);

int serial_intr_is_recv_enable(int index);
void serial_intr_recv_enable(int index);
void serial_intr_recv_disable(int index);

#endif

