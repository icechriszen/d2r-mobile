/* Graphics-only tests keep the host camera disabled. The real camera is
 * covered by host tests and device runs; accidentally entering that path here
 * is an error, rather than a silent success with fabricated matrices. */
#include <stdlib.h>
int dm_cam_mode(void) { return 0; }
int dm_cam_want_dir(float *x, float *z) { (void)x; (void)z; abort(); }
void dm_cam_publish(const float a[16], const float b[16], const float c[16], const float d[16], float x, float z) {
    (void)a; (void)b; (void)c; (void)d; (void)x; (void)z; abort();
}
float dm_cam_take_turn(void) { abort(); }
double dm_cam_turn_age(void) { abort(); }
float dm_cam_tilt_get(void) { abort(); }
void dm_cam_tilt_clamp(float lo, float hi) { (void)lo; (void)hi; abort(); }
int dm_cam_clearance(float x, float z, float *distance, float *px, float *pz) {
    (void)x; (void)z; (void)distance; (void)px; (void)pz; abort();
}
