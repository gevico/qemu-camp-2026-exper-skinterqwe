#ifndef GPGPU_CHAR_H
#define GPGPU_CHAR_H

struct gpgpu_dev;

int gpgpu_char_init(void);
void gpgpu_char_exit(void);
void gpgpu_char_set_dev(struct gpgpu_dev *dev);

#endif /* GPGPU_CHAR_H */
