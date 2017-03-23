#include "Marlin.h"
#include "Configuration.h"
#include "ConfigurationStore.h"
#include "language_all.h"
#include "mesh_bed_calibration.h"
#include "mesh_bed_leveling.h"
#include "stepper.h"
#include "ultralcd.h"

uint8_t world2machine_correction_mode;
float   world2machine_rotation_and_skew[2][2];
float   world2machine_rotation_and_skew_inv[2][2];
float   world2machine_shift[2];

// Weight of the Y coordinate for the least squares fitting of the bed induction sensor targets.
// Only used for the first row of the points, which may not befully in reach of the sensor.
#define WEIGHT_FIRST_ROW_X_HIGH (1.f)
#define WEIGHT_FIRST_ROW_X_LOW  (0.35f)
#define WEIGHT_FIRST_ROW_Y_HIGH (0.3f)
#define WEIGHT_FIRST_ROW_Y_LOW  (0.0f)

#define BED_ZERO_REF_X (- 22.f + X_PROBE_OFFSET_FROM_EXTRUDER)
#define BED_ZERO_REF_Y (- 0.6f + Y_PROBE_OFFSET_FROM_EXTRUDER)

// Scaling of the real machine axes against the programmed dimensions in the firmware.
// The correction is tiny, here around 0.5mm on 250mm length.
//#define MACHINE_AXIS_SCALE_X ((250.f - 0.5f) / 250.f)
//#define MACHINE_AXIS_SCALE_Y ((250.f - 0.5f) / 250.f)
#define MACHINE_AXIS_SCALE_X 1.f
#define MACHINE_AXIS_SCALE_Y 1.f

// 0.12 degrees equals to an offset of 0.5mm on 250mm length. 
#define BED_SKEW_ANGLE_MILD         (0.12f * M_PI / 180.f)
// 0.25 degrees equals to an offset of 1.1mm on 250mm length.
#define BED_SKEW_ANGLE_EXTREME      (0.25f * M_PI / 180.f)

#define BED_CALIBRATION_POINT_OFFSET_MAX_EUCLIDIAN  (0.8f)
#define BED_CALIBRATION_POINT_OFFSET_MAX_1ST_ROW_X  (0.8f)
#define BED_CALIBRATION_POINT_OFFSET_MAX_1ST_ROW_Y  (1.5f)

#define MIN_BED_SENSOR_POINT_RESPONSE_DMR           (2.0f)

//#define Y_MIN_POS_FOR_BED_CALIBRATION (MANUAL_Y_HOME_POS-0.2f)
#define Y_MIN_POS_FOR_BED_CALIBRATION (Y_MIN_POS)
// Distances toward the print bed edge may not be accurate.
#define Y_MIN_POS_CALIBRATION_POINT_ACCURATE (Y_MIN_POS + 3.f)
// When the measured point center is out of reach of the sensor, Y coordinate will be ignored
// by the Least Squares fitting and the X coordinate will be weighted low.
#define Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH (Y_MIN_POS - 0.5f)

// Positions of the bed reference points in the machine coordinates, referenced to the P.I.N.D.A sensor.
// The points are ordered in a zig-zag fashion to speed up the calibration.
const float bed_ref_points[] PROGMEM = {
    13.f  - BED_ZERO_REF_X,   6.4f - BED_ZERO_REF_Y,
    115.f - BED_ZERO_REF_X,   6.4f - BED_ZERO_REF_Y,
    216.f - BED_ZERO_REF_X,   6.4f - BED_ZERO_REF_Y,

    216.f - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y,
    115.f - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y,
    13.f  - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y,

    13.f  - BED_ZERO_REF_X, 202.4f - BED_ZERO_REF_Y,
    115.f - BED_ZERO_REF_X, 202.4f - BED_ZERO_REF_Y,
    216.f - BED_ZERO_REF_X, 202.4f - BED_ZERO_REF_Y
};

// Positions of the bed reference points in the machine coordinates, referenced to the P.I.N.D.A sensor.
// The points are the following: center front, center right, center rear, center left.
const float bed_ref_points_4[] PROGMEM = {
    115.f - BED_ZERO_REF_X,   6.4f - BED_ZERO_REF_Y,
    216.f - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y,
    115.f - BED_ZERO_REF_X, 202.4f - BED_ZERO_REF_Y,
    13.f  - BED_ZERO_REF_X, 104.4f - BED_ZERO_REF_Y
};

static inline float sqr(float x) { return x * x; }

// Weight of a point coordinate in a least squares optimization.
// The first row of points may not be fully reachable
// and the y values may be shortened a bit by the bed carriage
// pulling the belt up.
static inline float point_weight_x(const uint8_t i, const float &y)
{
    float w = 1.f;
    if (i < 3) {
        if (y >= Y_MIN_POS_CALIBRATION_POINT_ACCURATE) {
            w = WEIGHT_FIRST_ROW_X_HIGH;
        } else if (y < Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH) {
            // If the point is fully outside, give it some weight.
            w = WEIGHT_FIRST_ROW_X_LOW;
        } else {
            // Linearly interpolate the weight from 1 to WEIGHT_FIRST_ROW_X.
            float t = (y - Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH) / (Y_MIN_POS_CALIBRATION_POINT_ACCURATE - Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH);
            w = (1.f - t) * WEIGHT_FIRST_ROW_X_LOW + t * WEIGHT_FIRST_ROW_X_HIGH;
        }
    }
    return w;
}

// Weight of a point coordinate in a least squares optimization.
// The first row of points may not be fully reachable
// and the y values may be shortened a bit by the bed carriage
// pulling the belt up.
static inline float point_weight_y(const uint8_t i, const float &y)
{
    float w = 1.f;
    if (i < 3) {
        if (y >= Y_MIN_POS_CALIBRATION_POINT_ACCURATE) {
            w = WEIGHT_FIRST_ROW_Y_HIGH;
        } else if (y < Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH) {
            // If the point is fully outside, give it some weight.
            w = WEIGHT_FIRST_ROW_Y_LOW;
        } else {
            // Linearly interpolate the weight from 1 to WEIGHT_FIRST_ROW_X.
            float t = (y - Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH) / (Y_MIN_POS_CALIBRATION_POINT_ACCURATE - Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH);
            w = (1.f - t) * WEIGHT_FIRST_ROW_Y_LOW + t * WEIGHT_FIRST_ROW_Y_HIGH;
        }
    }
    return w;
}

// Non-Linear Least Squares fitting of the bed to the measured induction points
// using the Gauss-Newton method.
// This method will maintain a unity length of the machine axes,
// which is the correct approach if the sensor points are not measured precisely.
BedSkewOffsetDetectionResultType calculate_machine_skew_and_offset_LS(
    // Matrix of maximum 9 2D points (18 floats)
    const float  *measured_pts,
    uint8_t       npts,
    const float  *true_pts,
    // Resulting correction matrix.
    float        *vec_x,
    float        *vec_y,
    float        *cntr,
    // Temporary values, 49-18-(2*3)=25 floats
    //    , float *temp
    int8_t        verbosity_level
    )
{
    if (verbosity_level >= 10) {
        // Show the initial state, before the fitting.
        SERIAL_ECHOPGM("X vector, initial: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector, initial: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center, initial: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");

        for (uint8_t i = 0; i < npts; ++i) {
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM(" measured: (");
            MYSERIAL.print(measured_pts[i * 2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i * 2 + 1], 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2 + 1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(sqrt(
                sqr(pgm_read_float(true_pts + i * 2) - measured_pts[i * 2]) +
                sqr(pgm_read_float(true_pts + i * 2 + 1) - measured_pts[i * 2 + 1])), 5);
            SERIAL_ECHOLNPGM("");
        }
        delay_keep_alive(100);
    }

    // Run some iterations of the Gauss-Newton method of non-linear least squares.
    // Initial set of parameters:
    // X,Y offset
    cntr[0] = 0.f;
    cntr[1] = 0.f;
    // Rotation of the machine X axis from the bed X axis.
    float a1 = 0;
    // Rotation of the machine Y axis from the bed Y axis.
    float a2 = 0;
    for (int8_t iter = 0; iter < 100; ++iter) {
        float c1 = cos(a1) * MACHINE_AXIS_SCALE_X;
        float s1 = sin(a1) * MACHINE_AXIS_SCALE_X;
        float c2 = cos(a2) * MACHINE_AXIS_SCALE_Y;
        float s2 = sin(a2) * MACHINE_AXIS_SCALE_Y;
        // Prepare the Normal equation for the Gauss-Newton method.
        float A[4][4] = { 0.f };
        float b[4] = { 0.f };
        float acc;
        for (uint8_t r = 0; r < 4; ++r) {
            for (uint8_t c = 0; c < 4; ++c) {
                acc = 0;
                // J^T times J
                for (uint8_t i = 0; i < npts; ++i) {
                    // First for the residuum in the x axis:
                    if (r != 1 && c != 1) {
                        float a = 
                             (r == 0) ? 1.f :
                            ((r == 2) ? (-s1 * measured_pts[2 * i]) :
                                        (-c2 * measured_pts[2 * i + 1]));
                        float b = 
                             (c == 0) ? 1.f :
                            ((c == 2) ? (-s1 * measured_pts[2 * i]) :
                                        (-c2 * measured_pts[2 * i + 1]));
                        float w = point_weight_x(i, measured_pts[2 * i + 1]);
                        acc += a * b * w;
                    }
                    // Second for the residuum in the y axis. 
                    // The first row of the points have a low weight, because their position may not be known
                    // with a sufficient accuracy.
                    if (r != 0 && c != 0) {
                        float a = 
                             (r == 1) ? 1.f :
                            ((r == 2) ? ( c1 * measured_pts[2 * i]) :
                                        (-s2 * measured_pts[2 * i + 1]));
                        float b = 
                             (c == 1) ? 1.f :
                            ((c == 2) ? ( c1 * measured_pts[2 * i]) :
                                        (-s2 * measured_pts[2 * i + 1]));
                        float w = point_weight_y(i, measured_pts[2 * i + 1]);
                        acc += a * b * w;
                    }
                }
                A[r][c] = acc;
            }
            // J^T times f(x)
            acc = 0.f;
            for (uint8_t i = 0; i < npts; ++i) {
                {
                    float j = 
                         (r == 0) ? 1.f :
                        ((r == 1) ? 0.f :
                        ((r == 2) ? (-s1 * measured_pts[2 * i]) :
                                    (-c2 * measured_pts[2 * i + 1])));
                    float fx = c1 * measured_pts[2 * i] - s2 * measured_pts[2 * i + 1] + cntr[0] - pgm_read_float(true_pts + i * 2);
                    float w = point_weight_x(i, measured_pts[2 * i + 1]);
                    acc += j * fx * w;
                }
                {
                    float j = 
                         (r == 0) ? 0.f :
                        ((r == 1) ? 1.f :
                        ((r == 2) ? ( c1 * measured_pts[2 * i]) :
                                    (-s2 * measured_pts[2 * i + 1])));
                    float fy = s1 * measured_pts[2 * i] + c2 * measured_pts[2 * i + 1] + cntr[1] - pgm_read_float(true_pts + i * 2 + 1);
                    float w = point_weight_y(i, measured_pts[2 * i + 1]);
                    acc += j * fy * w;
                }
            }
            b[r] = -acc;
        }

        // Solve for h by a Gauss iteration method.
        float h[4] = { 0.f };
        for (uint8_t gauss_iter = 0; gauss_iter < 100; ++gauss_iter) {
            h[0] = (b[0] - A[0][1] * h[1] - A[0][2] * h[2] - A[0][3] * h[3]) / A[0][0];
            h[1] = (b[1] - A[1][0] * h[0] - A[1][2] * h[2] - A[1][3] * h[3]) / A[1][1];
            h[2] = (b[2] - A[2][0] * h[0] - A[2][1] * h[1] - A[2][3] * h[3]) / A[2][2];
            h[3] = (b[3] - A[3][0] * h[0] - A[3][1] * h[1] - A[3][2] * h[2]) / A[3][3];
        }

        // and update the current position with h.
        // It may be better to use the Levenberg-Marquart method here,
        // but because we are very close to the solution alread,
        // the simple Gauss-Newton non-linear Least Squares method works well enough.
        cntr[0] += h[0];
        cntr[1] += h[1];
        a1 += h[2];
        a2 += h[3];

        if (verbosity_level >= 20) {
            SERIAL_ECHOPGM("iteration: ");
            MYSERIAL.print(iter, 0);
            SERIAL_ECHOPGM("correction vector: ");
            MYSERIAL.print(h[0], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(h[1], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(h[2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(h[3], 5);
            SERIAL_ECHOLNPGM("");
            SERIAL_ECHOPGM("corrected x/y: ");
            MYSERIAL.print(cntr[0], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(cntr[0], 5);
            SERIAL_ECHOLNPGM("");
            SERIAL_ECHOPGM("corrected angles: ");
            MYSERIAL.print(180.f * a1 / M_PI, 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(180.f * a2 / M_PI, 5);
            SERIAL_ECHOLNPGM("");
        }
    }

    vec_x[0] =  cos(a1) * MACHINE_AXIS_SCALE_X;
    vec_x[1] =  sin(a1) * MACHINE_AXIS_SCALE_X;
    vec_y[0] = -sin(a2) * MACHINE_AXIS_SCALE_Y;
    vec_y[1] =  cos(a2) * MACHINE_AXIS_SCALE_Y;

    BedSkewOffsetDetectionResultType result = BED_SKEW_OFFSET_DETECTION_PERFECT;
    {
        float angleDiff = fabs(a2 - a1);
        if (angleDiff > BED_SKEW_ANGLE_MILD)
            result = (angleDiff > BED_SKEW_ANGLE_EXTREME) ?
                BED_SKEW_OFFSET_DETECTION_SKEW_EXTREME :
                BED_SKEW_OFFSET_DETECTION_SKEW_MILD;
        if (fabs(a1) > BED_SKEW_ANGLE_EXTREME ||
            fabs(a2) > BED_SKEW_ANGLE_EXTREME)
            result = BED_SKEW_OFFSET_DETECTION_SKEW_EXTREME;
    }

    if (verbosity_level >= 1) {
        SERIAL_ECHOPGM("correction angles: ");
        MYSERIAL.print(180.f * a1 / M_PI, 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(180.f * a2 / M_PI, 5);
        SERIAL_ECHOLNPGM("");
    }

    if (verbosity_level >= 10) {
        // Show the adjusted state, before the fitting.
        SERIAL_ECHOPGM("X vector new, inverted: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector new, inverted: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center new, inverted: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");
        delay_keep_alive(100);

        SERIAL_ECHOLNPGM("Error after correction: ");
    }

    // Measure the error after correction.
    for (uint8_t i = 0; i < npts; ++i) {
        float x = vec_x[0] * measured_pts[i * 2] + vec_y[0] * measured_pts[i * 2 + 1] + cntr[0];
        float y = vec_x[1] * measured_pts[i * 2] + vec_y[1] * measured_pts[i * 2 + 1] + cntr[1];
        float errX = sqr(pgm_read_float(true_pts + i * 2) - x);
        float errY = sqr(pgm_read_float(true_pts + i * 2 + 1) - y);
        float err = sqrt(errX + errY);
        if (i < 3) {
            float w = point_weight_y(i, measured_pts[2 * i + 1]);
            if (sqrt(errX) > BED_CALIBRATION_POINT_OFFSET_MAX_1ST_ROW_X ||
                (w != 0.f && sqrt(errY) > BED_CALIBRATION_POINT_OFFSET_MAX_1ST_ROW_Y))
                result = BED_SKEW_OFFSET_DETECTION_FITTING_FAILED;
        } else {
            if (err > BED_CALIBRATION_POINT_OFFSET_MAX_EUCLIDIAN)
                result = BED_SKEW_OFFSET_DETECTION_FITTING_FAILED;
        }
        if (verbosity_level >= 10) {
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM(" measured: (");
            MYSERIAL.print(measured_pts[i * 2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i * 2 + 1], 5);
            SERIAL_ECHOPGM("); corrected: (");
            MYSERIAL.print(x, 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(y, 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2 + 1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(err);
            SERIAL_ECHOLNPGM("");
        }
    }

    #if 0
    if (result == BED_SKEW_OFFSET_DETECTION_PERFECT && fabs(a1) < BED_SKEW_ANGLE_MILD && fabs(a2) < BED_SKEW_ANGLE_MILD) {
        if (verbosity_level > 0)
            SERIAL_ECHOLNPGM("Very little skew detected. Disabling skew correction.");
        // Just disable the skew correction.
        vec_x[0] = MACHINE_AXIS_SCALE_X;
        vec_x[1] = 0.f;
        vec_y[0] = 0.f;
        vec_y[1] = MACHINE_AXIS_SCALE_Y;
    }
    #else
    if (result == BED_SKEW_OFFSET_DETECTION_PERFECT) {
        if (verbosity_level > 0)
            SERIAL_ECHOLNPGM("Very little skew detected. Orthogonalizing the axes.");
        // Orthogonalize the axes.
        a1 = 0.5f * (a1 + a2);
        vec_x[0] =  cos(a1) * MACHINE_AXIS_SCALE_X;
        vec_x[1] =  sin(a1) * MACHINE_AXIS_SCALE_X;
        vec_y[0] = -sin(a1) * MACHINE_AXIS_SCALE_Y;
        vec_y[1] =  cos(a1) * MACHINE_AXIS_SCALE_Y;
        // Refresh the offset.
        cntr[0] = 0.f;
        cntr[1] = 0.f;
        float wx = 0.f;
        float wy = 0.f;
        for (int8_t i = 0; i < npts; ++ i) {
            float x = vec_x[0] * measured_pts[i * 2] + vec_y[0] * measured_pts[i * 2 + 1];
            float y = vec_x[1] * measured_pts[i * 2] + vec_y[1] * measured_pts[i * 2 + 1];
            float w = point_weight_x(i, y);
			cntr[0] += w * (pgm_read_float(true_pts + i * 2) - x);
			wx += w;
			if (verbosity_level >= 20) {
				MYSERIAL.print(i);
				SERIAL_ECHOLNPGM("");
				SERIAL_ECHOLNPGM("Weight_x:");
				MYSERIAL.print(w);
				SERIAL_ECHOLNPGM("");
				SERIAL_ECHOLNPGM("cntr[0]:");
				MYSERIAL.print(cntr[0]);
				SERIAL_ECHOLNPGM("");
				SERIAL_ECHOLNPGM("wx:");
				MYSERIAL.print(wx);
			}
            w = point_weight_y(i, y);
			cntr[1] += w * (pgm_read_float(true_pts + i * 2 + 1) - y);
			wy += w;

			if (verbosity_level >= 20) {
				SERIAL_ECHOLNPGM("");
				SERIAL_ECHOLNPGM("Weight_y:");
				MYSERIAL.print(w);
				SERIAL_ECHOLNPGM("");
				SERIAL_ECHOLNPGM("cntr[1]:");
				MYSERIAL.print(cntr[1]);
				SERIAL_ECHOLNPGM("");
				SERIAL_ECHOLNPGM("wy:");
				MYSERIAL.print(wy);
				SERIAL_ECHOLNPGM("");
				SERIAL_ECHOLNPGM("");
			}
		}
        cntr[0] /= wx;
        cntr[1] /= wy;
		if (verbosity_level >= 20) {
			SERIAL_ECHOLNPGM("");
			SERIAL_ECHOLNPGM("Final cntr values:");
			SERIAL_ECHOLNPGM("cntr[0]:");
			MYSERIAL.print(cntr[0]);
			SERIAL_ECHOLNPGM("");
			SERIAL_ECHOLNPGM("cntr[1]:");
			MYSERIAL.print(cntr[1]);
			SERIAL_ECHOLNPGM("");
		}

    }
    #endif

    // Invert the transformation matrix made of vec_x, vec_y and cntr.
    {
        float d = vec_x[0] * vec_y[1] - vec_x[1] * vec_y[0];
        float Ainv[2][2] = {
            { vec_y[1] / d, -vec_y[0] / d },
            { -vec_x[1] / d, vec_x[0] / d }
        };
        float cntrInv[2] = {
            -Ainv[0][0] * cntr[0] - Ainv[0][1] * cntr[1],
            -Ainv[1][0] * cntr[0] - Ainv[1][1] * cntr[1]
        };
        vec_x[0] = Ainv[0][0];
        vec_x[1] = Ainv[1][0];
        vec_y[0] = Ainv[0][1];
        vec_y[1] = Ainv[1][1];
        cntr[0] = cntrInv[0];
        cntr[1] = cntrInv[1];
    }

    if (verbosity_level >= 1) {
        // Show the adjusted state, before the fitting.
        SERIAL_ECHOPGM("X vector, adjusted: ");
        MYSERIAL.print(vec_x[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_x[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("Y vector, adjusted: ");
        MYSERIAL.print(vec_y[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(vec_y[1], 5);
        SERIAL_ECHOLNPGM("");

        SERIAL_ECHOPGM("center, adjusted: ");
        MYSERIAL.print(cntr[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(cntr[1], 5);
        SERIAL_ECHOLNPGM("");
        delay_keep_alive(100);
    }

    if (verbosity_level >= 2) {
        SERIAL_ECHOLNPGM("Difference after correction: ");
        for (uint8_t i = 0; i < npts; ++i) {
            float x = vec_x[0] * pgm_read_float(true_pts + i * 2) + vec_y[0] * pgm_read_float(true_pts + i * 2 + 1) + cntr[0];
            float y = vec_x[1] * pgm_read_float(true_pts + i * 2) + vec_y[1] * pgm_read_float(true_pts + i * 2 + 1) + cntr[1];
            SERIAL_ECHOPGM("point #");
            MYSERIAL.print(int(i));
            SERIAL_ECHOPGM("measured: (");
            MYSERIAL.print(measured_pts[i * 2], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(measured_pts[i * 2 + 1], 5);
            SERIAL_ECHOPGM("); measured-corrected: (");
            MYSERIAL.print(x, 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(y, 5);
            SERIAL_ECHOPGM("); target: (");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2), 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(pgm_read_float(true_pts + i * 2 + 1), 5);
            SERIAL_ECHOPGM("), error: ");
            MYSERIAL.print(sqrt(sqr(measured_pts[i * 2] - x) + sqr(measured_pts[i * 2 + 1] - y)));
            SERIAL_ECHOLNPGM("");
        }
        delay_keep_alive(100);
    }

    return result;
}

void reset_bed_offset_and_skew()
{
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_CENTER+0), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_CENTER+4), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_VEC_X +0), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_VEC_X +4), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_VEC_Y +0), 0x0FFFFFFFF);
    eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_VEC_Y +4), 0x0FFFFFFFF);

    // Reset the 8 16bit offsets.
    for (int8_t i = 0; i < 4; ++ i)
        eeprom_update_dword((uint32_t*)(EEPROM_BED_CALIBRATION_Z_JITTER+i*4), 0x0FFFFFFFF);
}

bool is_bed_z_jitter_data_valid()
// offsets of the Z heiths of the calibration points from the first point are saved as 16bit signed int, scaled to tenths of microns
{
    for (int8_t i = 0; i < 8; ++ i)
        if (eeprom_read_word((uint16_t*)(EEPROM_BED_CALIBRATION_Z_JITTER+i*2)) == 0x0FFFF)
            return false;
    return true;
}

static void world2machine_update(const float vec_x[2], const float vec_y[2], const float cntr[2])
{
    world2machine_rotation_and_skew[0][0] = vec_x[0];
    world2machine_rotation_and_skew[1][0] = vec_x[1];
    world2machine_rotation_and_skew[0][1] = vec_y[0];
    world2machine_rotation_and_skew[1][1] = vec_y[1];
    world2machine_shift[0] = cntr[0];
    world2machine_shift[1] = cntr[1];
    // No correction.
    world2machine_correction_mode = WORLD2MACHINE_CORRECTION_NONE;
    if (world2machine_shift[0] != 0.f || world2machine_shift[1] != 0.f)
        // Shift correction.
        world2machine_correction_mode |= WORLD2MACHINE_CORRECTION_SHIFT;
    if (world2machine_rotation_and_skew[0][0] != 1.f || world2machine_rotation_and_skew[0][1] != 0.f ||
        world2machine_rotation_and_skew[1][0] != 0.f || world2machine_rotation_and_skew[1][1] != 1.f) {
        // Rotation & skew correction.
        world2machine_correction_mode |= WORLD2MACHINE_CORRECTION_SKEW;
        // Invert the world2machine matrix.
        float d = world2machine_rotation_and_skew[0][0] * world2machine_rotation_and_skew[1][1] - world2machine_rotation_and_skew[1][0] * world2machine_rotation_and_skew[0][1];
        world2machine_rotation_and_skew_inv[0][0] =  world2machine_rotation_and_skew[1][1] / d;
        world2machine_rotation_and_skew_inv[0][1] = -world2machine_rotation_and_skew[0][1] / d;
        world2machine_rotation_and_skew_inv[1][0] = -world2machine_rotation_and_skew[1][0] / d;
        world2machine_rotation_and_skew_inv[1][1] =  world2machine_rotation_and_skew[0][0] / d;
    } else {
        world2machine_rotation_and_skew_inv[0][0] = 1.f;
        world2machine_rotation_and_skew_inv[0][1] = 0.f;
        world2machine_rotation_and_skew_inv[1][0] = 0.f;
        world2machine_rotation_and_skew_inv[1][1] = 1.f;
    }
}

void world2machine_reset()
{
    const float vx[] = { 1.f, 0.f };
    const float vy[] = { 0.f, 1.f };
    const float cntr[] = { 0.f, 0.f };
    world2machine_update(vx, vy, cntr);
}

void world2machine_revert_to_uncorrected()
{
    if (world2machine_correction_mode != WORLD2MACHINE_CORRECTION_NONE) {
        // Reset the machine correction matrix.
        const float vx[] = { 1.f, 0.f };
        const float vy[] = { 0.f, 1.f };
        const float cntr[] = { 0.f, 0.f };
        world2machine_update(vx, vy, cntr);
        // Wait for the motors to stop and update the current position with the absolute values.
        st_synchronize();
        current_position[X_AXIS] = st_get_position_mm(X_AXIS);
        current_position[Y_AXIS] = st_get_position_mm(Y_AXIS);
    }
}

static inline bool vec_undef(const float v[2])
{
    const uint32_t *vx = (const uint32_t*)v;
    return vx[0] == 0x0FFFFFFFF || vx[1] == 0x0FFFFFFFF;
}

void world2machine_initialize()
{
//    SERIAL_ECHOLNPGM("world2machine_initialize()");
    float cntr[2] = {
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_CENTER+0)),
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_CENTER+4))
    };
    float vec_x[2] = {
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +0)),
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +4))
    };
    float vec_y[2] = {
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +0)),
        eeprom_read_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +4))
    };

    bool reset = false;
    if (vec_undef(cntr) || vec_undef(vec_x) || vec_undef(vec_y)) {
        // SERIAL_ECHOLNPGM("Undefined bed correction matrix.");
        reset = true;
    }
    else {
        // Length of the vec_x shall be close to unity.
        float l = sqrt(vec_x[0] * vec_x[0] + vec_x[1] * vec_x[1]);
        if (l < 0.9 || l > 1.1) {
			SERIAL_ECHOLNPGM("X vector length:");
			MYSERIAL.println(l);
            SERIAL_ECHOLNPGM("Invalid bed correction matrix. Length of the X vector out of range.");
            reset = true;
        }
        // Length of the vec_y shall be close to unity.
        l = sqrt(vec_y[0] * vec_y[0] + vec_y[1] * vec_y[1]);
        if (l < 0.9 || l > 1.1) {
			SERIAL_ECHOLNPGM("Y vector length:");
			MYSERIAL.println(l);
            SERIAL_ECHOLNPGM("Invalid bed correction matrix. Length of the Y vector out of range.");
            reset = true;
        }
        // Correction of the zero point shall be reasonably small.
        l = sqrt(cntr[0] * cntr[0] + cntr[1] * cntr[1]);
        if (l > 15.f) {
			SERIAL_ECHOLNPGM("Zero point correction:");
			MYSERIAL.println(l);
            SERIAL_ECHOLNPGM("Invalid bed correction matrix. Shift out of range.");
            reset = true;
        }
        // vec_x and vec_y shall be nearly perpendicular.
        l = vec_x[0] * vec_y[0] + vec_x[1] * vec_y[1];
        if (fabs(l) > 0.1f) {
            SERIAL_ECHOLNPGM("Invalid bed correction matrix. X/Y axes are far from being perpendicular.");
            reset = true;
        }
    }

    if (reset) {
        SERIAL_ECHOLNPGM("Invalid bed correction matrix. Resetting to identity.");
        reset_bed_offset_and_skew();
        world2machine_reset();
    } else {
        world2machine_update(vec_x, vec_y, cntr);
        /*
        SERIAL_ECHOPGM("world2machine_initialize() loaded: ");
        MYSERIAL.print(world2machine_rotation_and_skew[0][0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(world2machine_rotation_and_skew[0][1], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(world2machine_rotation_and_skew[1][0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(world2machine_rotation_and_skew[1][1], 5);
        SERIAL_ECHOPGM(", offset ");
        MYSERIAL.print(world2machine_shift[0], 5);
        SERIAL_ECHOPGM(", ");
        MYSERIAL.print(world2machine_shift[1], 5);
        SERIAL_ECHOLNPGM("");
        */
    }
}

// When switching from absolute to corrected coordinates,
// this will get the absolute coordinates from the servos,
// applies the inverse world2machine transformation
// and stores the result into current_position[x,y].
void world2machine_update_current()
{
    float x = current_position[X_AXIS] - world2machine_shift[0];
    float y = current_position[Y_AXIS] - world2machine_shift[1];
    current_position[X_AXIS] = world2machine_rotation_and_skew_inv[0][0] * x + world2machine_rotation_and_skew_inv[0][1] * y;
    current_position[Y_AXIS] = world2machine_rotation_and_skew_inv[1][0] * x + world2machine_rotation_and_skew_inv[1][1] * y;
}

static inline void go_xyz(float x, float y, float z, float fr)
{
    plan_buffer_line(x, y, z, current_position[E_AXIS], fr, active_extruder);
    st_synchronize();
}

static inline void go_xy(float x, float y, float fr)
{
    plan_buffer_line(x, y, current_position[Z_AXIS], current_position[E_AXIS], fr, active_extruder);
    st_synchronize();
}

static inline void go_to_current(float fr)
{
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS], fr, active_extruder);
    st_synchronize();
}

static inline void update_current_position_xyz()
{
      current_position[X_AXIS] = st_get_position_mm(X_AXIS);
      current_position[Y_AXIS] = st_get_position_mm(Y_AXIS);
      current_position[Z_AXIS] = st_get_position_mm(Z_AXIS);
      plan_set_position(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], current_position[E_AXIS]);
}

static inline void update_current_position_z()
{
      current_position[Z_AXIS] = st_get_position_mm(Z_AXIS);
      plan_set_z_position(current_position[Z_AXIS]);
}

// At the current position, find the Z stop.
inline bool find_bed_induction_sensor_point_z(float minimum_z, uint8_t n_iter) 
{
//    SERIAL_ECHOLNPGM("find_bed_induction_sensor_point_z 1");
    bool endstops_enabled  = enable_endstops(true);
    bool endstop_z_enabled = enable_z_endstop(false);
    float z = 0.f;
    endstop_z_hit_on_purpose();

    // move down until you find the bed
    current_position[Z_AXIS] = minimum_z;
    go_to_current(homing_feedrate[Z_AXIS]/60);
    // we have to let the planner know where we are right now as it is not where we said to go.
    update_current_position_z();
    if (! endstop_z_hit_on_purpose())
        goto error;

    for (uint8_t i = 0; i < n_iter; ++ i) {
        // Move up the retract distance.
        current_position[Z_AXIS] += .5f;
        go_to_current(homing_feedrate[Z_AXIS]/60);
        // Move back down slowly to find bed.
        current_position[Z_AXIS] = minimum_z;
        go_to_current(homing_feedrate[Z_AXIS]/(4*60));
        // we have to let the planner know where we are right now as it is not where we said to go.
        update_current_position_z();
        if (! endstop_z_hit_on_purpose())
            goto error;
//        SERIAL_ECHOPGM("Bed find_bed_induction_sensor_point_z low, height: ");
//        MYSERIAL.print(current_position[Z_AXIS], 5);
//        SERIAL_ECHOLNPGM("");
        z += current_position[Z_AXIS];
    }
    current_position[Z_AXIS] = z;
    if (n_iter > 1)
        current_position[Z_AXIS] /= float(n_iter);

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
//    SERIAL_ECHOLNPGM("find_bed_induction_sensor_point_z 3");
    return true;

error:
//    SERIAL_ECHOLNPGM("find_bed_induction_sensor_point_z 4");
    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return false;
}

// Search around the current_position[X,Y],
// look for the induction sensor response.
// Adjust the  current_position[X,Y,Z] to the center of the target dot and its response Z coordinate.
#define FIND_BED_INDUCTION_SENSOR_POINT_X_RADIUS (8.f)
#define FIND_BED_INDUCTION_SENSOR_POINT_Y_RADIUS (6.f)
#define FIND_BED_INDUCTION_SENSOR_POINT_XY_STEP  (1.f)
#define FIND_BED_INDUCTION_SENSOR_POINT_Z_STEP   (0.2f)
inline bool find_bed_induction_sensor_point_xy()
{
    float feedrate = homing_feedrate[X_AXIS] / 60.f;
    bool found = false;

    {
        float x0 = current_position[X_AXIS] - FIND_BED_INDUCTION_SENSOR_POINT_X_RADIUS;
        float x1 = current_position[X_AXIS] + FIND_BED_INDUCTION_SENSOR_POINT_X_RADIUS;
        float y0 = current_position[Y_AXIS] - FIND_BED_INDUCTION_SENSOR_POINT_Y_RADIUS;
        float y1 = current_position[Y_AXIS] + FIND_BED_INDUCTION_SENSOR_POINT_Y_RADIUS;
        uint8_t nsteps_y;
        uint8_t i;
        if (x0 < X_MIN_POS)
            x0 = X_MIN_POS;
        if (x1 > X_MAX_POS)
            x1 = X_MAX_POS;
        if (y0 < Y_MIN_POS_FOR_BED_CALIBRATION)
            y0 = Y_MIN_POS_FOR_BED_CALIBRATION;
        if (y1 > Y_MAX_POS)
            y1 = Y_MAX_POS;
        nsteps_y = int(ceil((y1 - y0) / FIND_BED_INDUCTION_SENSOR_POINT_XY_STEP));

        enable_endstops(false);
        bool  dir_positive = true;

//        go_xyz(current_position[X_AXIS], current_position[Y_AXIS], MESH_HOME_Z_SEARCH, homing_feedrate[Z_AXIS]/60);
        go_xyz(x0, y0, current_position[Z_AXIS], feedrate);
        // Continously lower the Z axis.
        endstops_hit_on_purpose();
        enable_z_endstop(true);
        while (current_position[Z_AXIS] > -10.f) {
            // Do nsteps_y zig-zag movements.
            current_position[Y_AXIS] = y0;
            for (i = 0; i < nsteps_y; current_position[Y_AXIS] += (y1 - y0) / float(nsteps_y - 1), ++ i) {
                // Run with a slightly decreasing Z axis, zig-zag movement. Stop at the Z end-stop.
                current_position[Z_AXIS] -= FIND_BED_INDUCTION_SENSOR_POINT_Z_STEP / float(nsteps_y);
                go_xyz(dir_positive ? x1 : x0, current_position[Y_AXIS], current_position[Z_AXIS], feedrate);
                dir_positive = ! dir_positive;
                if (endstop_z_hit_on_purpose())
                    goto endloop;
            }
            for (i = 0; i < nsteps_y; current_position[Y_AXIS] -= (y1 - y0) / float(nsteps_y - 1), ++ i) {
                // Run with a slightly decreasing Z axis, zig-zag movement. Stop at the Z end-stop.
                current_position[Z_AXIS] -= FIND_BED_INDUCTION_SENSOR_POINT_Z_STEP / float(nsteps_y);
                go_xyz(dir_positive ? x1 : x0, current_position[Y_AXIS], current_position[Z_AXIS], feedrate);
                dir_positive = ! dir_positive;
                if (endstop_z_hit_on_purpose())
                    goto endloop;
            }
        }
        endloop:
//        SERIAL_ECHOLN("First hit");

        // we have to let the planner know where we are right now as it is not where we said to go.
        update_current_position_xyz();

        // Search in this plane for the first hit. Zig-zag first in X, then in Y axis.
        for (int8_t iter = 0; iter < 3; ++ iter) {
            if (iter > 0) {
                // Slightly lower the Z axis to get a reliable trigger.
                current_position[Z_AXIS] -= 0.02f;
                go_xyz(current_position[X_AXIS], current_position[Y_AXIS], MESH_HOME_Z_SEARCH, homing_feedrate[Z_AXIS]/60);
            }

            // Do nsteps_y zig-zag movements.
            float a, b;
            enable_endstops(false);
            enable_z_endstop(false);
            current_position[Y_AXIS] = y0;
            go_xy(x0, current_position[Y_AXIS], feedrate);
            enable_z_endstop(true);
            found = false;
            for (i = 0, dir_positive = true; i < nsteps_y; current_position[Y_AXIS] += (y1 - y0) / float(nsteps_y - 1), ++ i, dir_positive = ! dir_positive) {
                go_xy(dir_positive ? x1 : x0, current_position[Y_AXIS], feedrate);
                if (endstop_z_hit_on_purpose()) {
                    found = true;
                    break;
                }
            }
            update_current_position_xyz();
            if (! found) {
//                SERIAL_ECHOLN("Search in Y - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search in Y - found");
            a = current_position[Y_AXIS];

            enable_z_endstop(false);
            current_position[Y_AXIS] = y1;
            go_xy(x0, current_position[Y_AXIS], feedrate);
            enable_z_endstop(true);
            found = false;
            for (i = 0, dir_positive = true; i < nsteps_y; current_position[Y_AXIS] -= (y1 - y0) / float(nsteps_y - 1), ++ i, dir_positive = ! dir_positive) {
                go_xy(dir_positive ? x1 : x0, current_position[Y_AXIS], feedrate);
                if (endstop_z_hit_on_purpose()) {
                    found = true;
                    break;
                }
            }
            update_current_position_xyz();
            if (! found) {
//                SERIAL_ECHOLN("Search in Y2 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search in Y2 - found");
            b = current_position[Y_AXIS];
            current_position[Y_AXIS] = 0.5f * (a + b);

            // Search in the X direction along a cross.
            found = false;
            enable_z_endstop(false);
            go_xy(x0, current_position[Y_AXIS], feedrate);
            enable_z_endstop(true);
            go_xy(x1, current_position[Y_AXIS], feedrate);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
//                SERIAL_ECHOLN("Search X span 0 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search X span 0 - found");
            a = current_position[X_AXIS];
            enable_z_endstop(false);
            go_xy(x1, current_position[Y_AXIS], feedrate);
            enable_z_endstop(true);
            go_xy(x0, current_position[Y_AXIS], feedrate);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
//                SERIAL_ECHOLN("Search X span 1 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search X span 1 - found");
            b = current_position[X_AXIS];
            // Go to the center.
            enable_z_endstop(false);
            current_position[X_AXIS] = 0.5f * (a + b);
            go_xy(current_position[X_AXIS], current_position[Y_AXIS], feedrate);
            found = true;

#if 1
            // Search in the Y direction along a cross.
            found = false;
            enable_z_endstop(false);
            go_xy(current_position[X_AXIS], y0, feedrate);
            enable_z_endstop(true);
            go_xy(current_position[X_AXIS], y1, feedrate);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
//                SERIAL_ECHOLN("Search Y2 span 0 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search Y2 span 0 - found");
            a = current_position[Y_AXIS];
            enable_z_endstop(false);
            go_xy(current_position[X_AXIS], y1, feedrate);
            enable_z_endstop(true);
            go_xy(current_position[X_AXIS], y0, feedrate);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
//                SERIAL_ECHOLN("Search Y2 span 1 - not found");
                continue;
            }
//            SERIAL_ECHOLN("Search Y2 span 1 - found");
            b = current_position[Y_AXIS];
            // Go to the center.
            enable_z_endstop(false);
            current_position[Y_AXIS] = 0.5f * (a + b);
            go_xy(current_position[X_AXIS], current_position[Y_AXIS], feedrate);
            found = true;
#endif
            break;
        }
    }

    enable_z_endstop(false);
    return found;
}

// Search around the current_position[X,Y,Z].
// It is expected, that the induction sensor is switched on at the current position.
// Look around this center point by painting a star around the point.
inline bool improve_bed_induction_sensor_point()
{
    static const float search_radius = 8.f;

    bool  endstops_enabled  = enable_endstops(false);
    bool  endstop_z_enabled = enable_z_endstop(false);
    bool  found = false;
    float feedrate = homing_feedrate[X_AXIS] / 60.f;
    float center_old_x = current_position[X_AXIS];
    float center_old_y = current_position[Y_AXIS];
    float center_x = 0.f;
    float center_y = 0.f;

    for (uint8_t iter = 0; iter < 4; ++ iter) {
        switch (iter) {
        case 0:
            destination[X_AXIS] = center_old_x - search_radius * 0.707;
            destination[Y_AXIS] = center_old_y - search_radius * 0.707;
            break;
        case 1:
            destination[X_AXIS] = center_old_x + search_radius * 0.707;
            destination[Y_AXIS] = center_old_y + search_radius * 0.707;
            break;
        case 2:
            destination[X_AXIS] = center_old_x + search_radius * 0.707;
            destination[Y_AXIS] = center_old_y - search_radius * 0.707;
            break;
        case 3:
        default:
            destination[X_AXIS] = center_old_x - search_radius * 0.707;
            destination[Y_AXIS] = center_old_y + search_radius * 0.707;
            break;
        }

        // Trim the vector from center_old_[x,y] to destination[x,y] by the bed dimensions.
        float vx = destination[X_AXIS] - center_old_x;
        float vy = destination[Y_AXIS] - center_old_y;
        float l  = sqrt(vx*vx+vy*vy);
        float t;
        if (destination[X_AXIS] < X_MIN_POS) {
            // Exiting the bed at xmin.
            t = (center_x - X_MIN_POS) / l;
            destination[X_AXIS] = X_MIN_POS;
            destination[Y_AXIS] = center_old_y + t * vy;
        } else if (destination[X_AXIS] > X_MAX_POS) {
            // Exiting the bed at xmax.
            t = (X_MAX_POS - center_x) / l;
            destination[X_AXIS] = X_MAX_POS;
            destination[Y_AXIS] = center_old_y + t * vy;
        }
        if (destination[Y_AXIS] < Y_MIN_POS_FOR_BED_CALIBRATION) {
            // Exiting the bed at ymin.
            t = (center_y - Y_MIN_POS_FOR_BED_CALIBRATION) / l;
            destination[X_AXIS] = center_old_x + t * vx;
            destination[Y_AXIS] = Y_MIN_POS_FOR_BED_CALIBRATION;
        } else if (destination[Y_AXIS] > Y_MAX_POS) {
            // Exiting the bed at xmax.
            t = (Y_MAX_POS - center_y) / l;
            destination[X_AXIS] = center_old_x + t * vx;
            destination[Y_AXIS] = Y_MAX_POS;
        }

        // Move away from the measurement point.
        enable_endstops(false);
        go_xy(destination[X_AXIS], destination[Y_AXIS], feedrate);
        // Move towards the measurement point, until the induction sensor triggers.
        enable_endstops(true);
        go_xy(center_old_x, center_old_y, feedrate);
        update_current_position_xyz();
//        if (! endstop_z_hit_on_purpose()) return false;
        center_x += current_position[X_AXIS];
        center_y += current_position[Y_AXIS];
    }

    // Calculate the new center, move to the new center.
    center_x /= 4.f;
    center_y /= 4.f;
    current_position[X_AXIS] = center_x;
    current_position[Y_AXIS] = center_y;
    enable_endstops(false);
    go_xy(current_position[X_AXIS], current_position[Y_AXIS], feedrate);

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return found;
}

static inline void debug_output_point(const char *type, const float &x, const float &y, const float &z)
{
    SERIAL_ECHOPGM("Measured ");
    SERIAL_ECHORPGM(type);
    SERIAL_ECHOPGM(" ");
    MYSERIAL.print(x, 5);
    SERIAL_ECHOPGM(", ");
    MYSERIAL.print(y, 5);
    SERIAL_ECHOPGM(", ");
    MYSERIAL.print(z, 5);
    SERIAL_ECHOLNPGM("");
}

// Search around the current_position[X,Y,Z].
// It is expected, that the induction sensor is switched on at the current position.
// Look around this center point by painting a star around the point.
#define IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS (8.f)
inline bool improve_bed_induction_sensor_point2(bool lift_z_on_min_y, int8_t verbosity_level)
{
    float center_old_x = current_position[X_AXIS];
    float center_old_y = current_position[Y_AXIS];
    float a, b;
    bool  point_small = false;

    enable_endstops(false);

    {
        float x0 = center_old_x - IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        float x1 = center_old_x + IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        if (x0 < X_MIN_POS)
            x0 = X_MIN_POS;
        if (x1 > X_MAX_POS)
            x1 = X_MAX_POS;

        // Search in the X direction along a cross.
        enable_z_endstop(false);
        go_xy(x0, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x1, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (! endstop_z_hit_on_purpose()) {
            current_position[X_AXIS] = center_old_x;
            goto canceled;
        }
        a = current_position[X_AXIS];
        enable_z_endstop(false);
        go_xy(x1, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x0, current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (! endstop_z_hit_on_purpose()) {
            current_position[X_AXIS] = center_old_x;
            goto canceled;
        }
        b = current_position[X_AXIS];
        if (b - a < MIN_BED_SENSOR_POINT_RESPONSE_DMR) {
            if (verbosity_level >= 5) {
                SERIAL_ECHOPGM("Point width too small: ");
                SERIAL_ECHO(b - a);
                SERIAL_ECHOLNPGM("");
            }
            // We force the calibration routine to move the Z axis slightly down to make the response more pronounced.
            if (b - a < 0.5f * MIN_BED_SENSOR_POINT_RESPONSE_DMR) {
                // Don't use the new X value.
                current_position[X_AXIS] = center_old_x;
                goto canceled;
            } else {
                // Use the new value, but force the Z axis to go a bit lower.
                point_small = true;
            }
        }
        if (verbosity_level >= 5) {
            debug_output_point(PSTR("left" ), a, current_position[Y_AXIS], current_position[Z_AXIS]);
            debug_output_point(PSTR("right"), b, current_position[Y_AXIS], current_position[Z_AXIS]);
        }

        // Go to the center.
        enable_z_endstop(false);
        current_position[X_AXIS] = 0.5f * (a + b);
        go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    }

    {
        float y0 = center_old_y - IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        float y1 = center_old_y + IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS;
        if (y0 < Y_MIN_POS_FOR_BED_CALIBRATION)
            y0 = Y_MIN_POS_FOR_BED_CALIBRATION;
        if (y1 > Y_MAX_POS)
            y1 = Y_MAX_POS;

        // Search in the Y direction along a cross.
        enable_z_endstop(false);
        go_xy(current_position[X_AXIS], y0, homing_feedrate[X_AXIS] / 60.f);
        if (lift_z_on_min_y) {
            // The first row of points are very close to the end stop.
            // Lift the sensor to disengage the trigger. This is necessary because of the sensor hysteresis.
            go_xyz(current_position[X_AXIS], y0, current_position[Z_AXIS]+1.5f, homing_feedrate[Z_AXIS] / 60.f);
            // and go back.
            go_xyz(current_position[X_AXIS], y0, current_position[Z_AXIS], homing_feedrate[Z_AXIS] / 60.f);
        }
        if (lift_z_on_min_y && (READ(Z_MIN_PIN) ^ Z_MIN_ENDSTOP_INVERTING) == 1) {
            // Already triggering before we started the move.
            // Shift the trigger point slightly outwards.
            // a = current_position[Y_AXIS] - 1.5f;
            a = current_position[Y_AXIS];
        } else {
            enable_z_endstop(true);
            go_xy(current_position[X_AXIS], y1, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                current_position[Y_AXIS] = center_old_y;
                goto canceled;
            }
            a = current_position[Y_AXIS];
        }
        enable_z_endstop(false);
        go_xy(current_position[X_AXIS], y1, homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(current_position[X_AXIS], y0, homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (! endstop_z_hit_on_purpose()) {
            current_position[Y_AXIS] = center_old_y;
            goto canceled;
        }
        b = current_position[Y_AXIS];
        if (b - a < MIN_BED_SENSOR_POINT_RESPONSE_DMR) {
            // We force the calibration routine to move the Z axis slightly down to make the response more pronounced.
            if (verbosity_level >= 5) {
                SERIAL_ECHOPGM("Point height too small: ");
                SERIAL_ECHO(b - a);
                SERIAL_ECHOLNPGM("");
            }
            if (b - a < 0.5f * MIN_BED_SENSOR_POINT_RESPONSE_DMR) {
                // Don't use the new Y value.
                current_position[Y_AXIS] = center_old_y;
                goto canceled;
            } else {
                // Use the new value, but force the Z axis to go a bit lower.
                point_small = true;
            }
        }
        if (verbosity_level >= 5) {
            debug_output_point(PSTR("top" ), current_position[X_AXIS], a, current_position[Z_AXIS]);
            debug_output_point(PSTR("bottom"), current_position[X_AXIS], b, current_position[Z_AXIS]);
        }

        // Go to the center.
        enable_z_endstop(false);
        current_position[Y_AXIS] = 0.5f * (a + b);
        go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    }

    // If point is small but not too small, then force the Z axis to be lowered a bit,
    // but use the new value. This is important when the initial position was off in one axis,
    // for example if the initial calibration was shifted in the Y axis systematically.
    // Then this first step will center.
    return ! point_small;

canceled:
    // Go back to the center.
    enable_z_endstop(false);
    go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    return false;
}

// Searching the front points, where one cannot move the sensor head in front of the sensor point.
// Searching in a zig-zag movement in a plane for the maximum width of the response.
// This function may set the current_position[Y_AXIS] below Y_MIN_POS, if the function succeeded.
// If this function failed, the Y coordinate will never be outside the working space.
#define IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS (4.f)
#define IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y (0.1f)
inline bool improve_bed_induction_sensor_point3(int verbosity_level)
{
    float center_old_x = current_position[X_AXIS];
    float center_old_y = current_position[Y_AXIS];
    float a, b;
    bool  result = true;

    // Was the sensor point detected too far in the minus Y axis?
    // If yes, the center of the induction point cannot be reached by the machine.
    {
        float x0 = center_old_x - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
        float x1 = center_old_x + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
        float y0 = center_old_y - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
        float y1 = center_old_y + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
        float y = y0;

        if (x0 < X_MIN_POS)
            x0 = X_MIN_POS;
        if (x1 > X_MAX_POS)
            x1 = X_MAX_POS;
        if (y0 < Y_MIN_POS_FOR_BED_CALIBRATION)
            y0 = Y_MIN_POS_FOR_BED_CALIBRATION;
        if (y1 > Y_MAX_POS)
            y1 = Y_MAX_POS;

        if (verbosity_level >= 20) {
            SERIAL_ECHOPGM("Initial position: ");
            SERIAL_ECHO(center_old_x);
            SERIAL_ECHOPGM(", ");
            SERIAL_ECHO(center_old_y);
            SERIAL_ECHOLNPGM("");
        }

        // Search in the positive Y direction, until a maximum diameter is found.
        // (the next diameter is smaller than the current one.)
        float dmax = 0.f;
        float xmax1 = 0.f;
        float xmax2 = 0.f;
        for (y = y0; y < y1; y += IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y) {
            enable_z_endstop(false);
            go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                continue;
                // SERIAL_PROTOCOLPGM("Failed 1\n");
                // current_position[X_AXIS] = center_old_x;
                // goto canceled;
            }
            a = current_position[X_AXIS];
            enable_z_endstop(false);
            go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                continue;
                // SERIAL_PROTOCOLPGM("Failed 2\n");
                // current_position[X_AXIS] = center_old_x;
                // goto canceled;
            }
            b = current_position[X_AXIS];
            if (verbosity_level >= 5) {
                debug_output_point(PSTR("left" ), a, current_position[Y_AXIS], current_position[Z_AXIS]);
                debug_output_point(PSTR("right"), b, current_position[Y_AXIS], current_position[Z_AXIS]);
            }
            float d = b - a;
            if (d > dmax) {
                xmax1 = 0.5f * (a + b);
                dmax = d;
            } else if (dmax > 0.) {
                y0 = y - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y;
                break;
            }
        }
        if (dmax == 0.) {
            if (verbosity_level > 0)
                SERIAL_PROTOCOLPGM("failed - not found\n");
            current_position[X_AXIS] = center_old_x;
            current_position[Y_AXIS] = center_old_y;
            goto canceled;
        }

        {
            // Find the positive Y hit. This gives the extreme Y value for the search of the maximum diameter in the -Y direction.
            enable_z_endstop(false);
            go_xy(xmax1, y0 + IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(xmax1, max(y0 - IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS, Y_MIN_POS_FOR_BED_CALIBRATION), homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                current_position[Y_AXIS] = center_old_y;
                goto canceled;
            }
            if (verbosity_level >= 5)
                debug_output_point(PSTR("top" ), current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS]);
            y1 = current_position[Y_AXIS];
        }

        if (y1 <= y0) {
            // Either the induction sensor is too high, or the induction sensor target is out of reach.
            current_position[Y_AXIS] = center_old_y;
            goto canceled;
        }

        // Search in the negative Y direction, until a maximum diameter is found.
        dmax = 0.f;
        // if (y0 + 1.f < y1)
        //    y1 = y0 + 1.f;
        for (y = y1; y >= y0; y -= IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y) {
            enable_z_endstop(false);
            go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                continue;
                /*
                current_position[X_AXIS] = center_old_x;
                SERIAL_PROTOCOLPGM("Failed 3\n");
                goto canceled;
                */
            }
            a = current_position[X_AXIS];
            enable_z_endstop(false);
            go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                continue;
                /*
                current_position[X_AXIS] = center_old_x;
                SERIAL_PROTOCOLPGM("Failed 4\n");
                goto canceled;
                */
            }
            b = current_position[X_AXIS];
            if (verbosity_level >= 5) {
                debug_output_point(PSTR("left" ), a, current_position[Y_AXIS], current_position[Z_AXIS]);
                debug_output_point(PSTR("right"), b, current_position[Y_AXIS], current_position[Z_AXIS]);
            }
            float d = b - a;
            if (d > dmax) {
                xmax2 = 0.5f * (a + b);
                dmax = d;
            } else if (dmax > 0.) {
                y1 = y + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y;
                break;
            }
        }
        float xmax, ymax;
        if (dmax == 0.f) {
            // Only the hit in the positive direction found.
            xmax = xmax1;
            ymax = y0;
        } else {
            // Both positive and negative directions found.
            xmax = xmax2;
            ymax = 0.5f * (y0 + y1);
            for (; y >= y0; y -= IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y) {
                enable_z_endstop(false);
                go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
                enable_z_endstop(true);
                go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
                update_current_position_xyz();
                if (! endstop_z_hit_on_purpose()) {
                    continue;
                    /*
                    current_position[X_AXIS] = center_old_x;
                    SERIAL_PROTOCOLPGM("Failed 3\n");
                    goto canceled;
                    */
                }
                a = current_position[X_AXIS];
                enable_z_endstop(false);
                go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
                enable_z_endstop(true);
                go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
                update_current_position_xyz();
                if (! endstop_z_hit_on_purpose()) {
                    continue;
                    /*
                    current_position[X_AXIS] = center_old_x;
                    SERIAL_PROTOCOLPGM("Failed 4\n");
                    goto canceled;
                    */
                }
                b = current_position[X_AXIS];
                if (verbosity_level >= 5) {
                    debug_output_point(PSTR("left" ), a, current_position[Y_AXIS], current_position[Z_AXIS]);
                    debug_output_point(PSTR("right"), b, current_position[Y_AXIS], current_position[Z_AXIS]);
                }
                float d = b - a;
                if (d > dmax) {
                    xmax = 0.5f * (a + b);
                    ymax = y;
                    dmax = d;
                }
            }
        }

        {
            // Compare the distance in the Y+ direction with the diameter in the X direction.
            // Find the positive Y hit once again, this time along the Y axis going through the X point with the highest diameter.
            enable_z_endstop(false);
            go_xy(xmax, ymax + IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS, homing_feedrate[X_AXIS] / 60.f);
            enable_z_endstop(true);
            go_xy(xmax, max(ymax - IMPROVE_BED_INDUCTION_SENSOR_SEARCH_RADIUS, Y_MIN_POS_FOR_BED_CALIBRATION), homing_feedrate[X_AXIS] / 60.f);
            update_current_position_xyz();
            if (! endstop_z_hit_on_purpose()) {
                current_position[Y_AXIS] = center_old_y;
                goto canceled;
            }
            if (verbosity_level >= 5)
                debug_output_point(PSTR("top" ), current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS]);
            if (current_position[Y_AXIS] - Y_MIN_POS_FOR_BED_CALIBRATION < 0.5f * dmax) {
                // Probably not even a half circle was detected. The induction point is likely too far in the minus Y direction.
                // First verify, if the measurement has been done at a sufficient height. If no, lower the Z axis a bit.
                if (current_position[Y_AXIS] < ymax || dmax < 0.5f * MIN_BED_SENSOR_POINT_RESPONSE_DMR) {
                    if (verbosity_level >= 5) {
                        SERIAL_ECHOPGM("Partial point diameter too small: ");
                        SERIAL_ECHO(dmax);
                        SERIAL_ECHOLNPGM("");
                    }
                    result = false;
                } else {
                    // Estimate the circle radius from the maximum diameter and height:
                    float h = current_position[Y_AXIS] - ymax;
                    float r = dmax * dmax / (8.f * h) + 0.5f * h;
                    if (r < 0.8f * MIN_BED_SENSOR_POINT_RESPONSE_DMR) {
                        if (verbosity_level >= 5) {
                            SERIAL_ECHOPGM("Partial point estimated radius too small: ");
                            SERIAL_ECHO(r);
                            SERIAL_ECHOPGM(", dmax:");
                            SERIAL_ECHO(dmax);
                            SERIAL_ECHOPGM(", h:");
                            SERIAL_ECHO(h);
                            SERIAL_ECHOLNPGM("");
                        }
                        result = false;
                    } else {
                        // The point may end up outside of the machine working space.
                        // That is all right as it helps to improve the accuracy of the measurement point
                        // due to averaging.
                        // For the y correction, use an average of dmax/2 and the estimated radius.
                        r = 0.5f * (0.5f * dmax + r);
                        ymax = current_position[Y_AXIS] - r;
                    }
                }
            } else {
                // If the diameter of the detected spot was smaller than a minimum allowed,
                // the induction sensor is probably too high. Returning false will force
                // the sensor to be lowered a tiny bit.
                result = xmax >= MIN_BED_SENSOR_POINT_RESPONSE_DMR;
                if (y0 > Y_MIN_POS_FOR_BED_CALIBRATION + 0.2f)
                    // Only in case both left and right y tangents are known, use them.
                    // If y0 is close to the bed edge, it may not be symmetric to the right tangent.
                    ymax = 0.5f * ymax + 0.25f * (y0 + y1);
            }
        }

        // Go to the center.
        enable_z_endstop(false);
        current_position[X_AXIS] = xmax;
        current_position[Y_AXIS] = ymax;
        if (verbosity_level >= 20) {
            SERIAL_ECHOPGM("Adjusted position: ");
            SERIAL_ECHO(current_position[X_AXIS]);
            SERIAL_ECHOPGM(", ");
            SERIAL_ECHO(current_position[Y_AXIS]);
            SERIAL_ECHOLNPGM("");
        }

        // Don't clamp current_position[Y_AXIS], because the out-of-reach Y coordinate may actually be true.
        // Only clamp the coordinate to go.
        go_xy(current_position[X_AXIS], max(Y_MIN_POS, current_position[Y_AXIS]), homing_feedrate[X_AXIS] / 60.f);
        // delay_keep_alive(3000);
    }

    if (result)
        return true;
    // otherwise clamp the Y coordinate

canceled:
    // Go back to the center.
    enable_z_endstop(false);
    if (current_position[Y_AXIS] < Y_MIN_POS)
        current_position[Y_AXIS] = Y_MIN_POS;
    go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
    return false;
}

// Scan the mesh bed induction points one by one by a left-right zig-zag movement,
// write the trigger coordinates to the serial line.
// Useful for visualizing the behavior of the bed induction detector.
inline void scan_bed_induction_sensor_point()
{
    float center_old_x = current_position[X_AXIS];
    float center_old_y = current_position[Y_AXIS];
    float x0 = center_old_x - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
    float x1 = center_old_x + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
    float y0 = center_old_y - IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
    float y1 = center_old_y + IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_RADIUS;
    float y = y0;

    if (x0 < X_MIN_POS)
        x0 = X_MIN_POS;
    if (x1 > X_MAX_POS)
        x1 = X_MAX_POS;
    if (y0 < Y_MIN_POS_FOR_BED_CALIBRATION)
        y0 = Y_MIN_POS_FOR_BED_CALIBRATION;
    if (y1 > Y_MAX_POS)
        y1 = Y_MAX_POS;

    for (float y = y0; y < y1; y += IMPROVE_BED_INDUCTION_SENSOR_POINT3_SEARCH_STEP_FINE_Y) {
        enable_z_endstop(false);
        go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (endstop_z_hit_on_purpose())
            debug_output_point(PSTR("left" ), current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS]);
        enable_z_endstop(false);
        go_xy(x1, y, homing_feedrate[X_AXIS] / 60.f);
        enable_z_endstop(true);
        go_xy(x0, y, homing_feedrate[X_AXIS] / 60.f);
        update_current_position_xyz();
        if (endstop_z_hit_on_purpose())
            debug_output_point(PSTR("right"), current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS]);
    }

    enable_z_endstop(false);
    current_position[X_AXIS] = center_old_x;
    current_position[Y_AXIS] = center_old_y;
    go_xy(current_position[X_AXIS], current_position[Y_AXIS], homing_feedrate[X_AXIS] / 60.f);
}

#define MESH_BED_CALIBRATION_SHOW_LCD

BedSkewOffsetDetectionResultType find_bed_offset_and_skew(int8_t verbosity_level)
{
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Reusing the z_values memory for the measurement cache.
    // 7x7=49 floats, good for 16 (x,y,z) vectors.
    float *pts = &mbl.z_values[0][0];
    float *vec_x = pts + 2 * 4;
    float *vec_y = vec_x + 2;
    float *cntr  = vec_y + 2;
    memset(pts, 0, sizeof(float) * 7 * 7);

//    SERIAL_ECHOLNPGM("find_bed_offset_and_skew verbosity level: ");
//    SERIAL_ECHO(int(verbosity_level));
//    SERIAL_ECHOPGM("");

#ifdef MESH_BED_CALIBRATION_SHOW_LCD
    uint8_t next_line;
    lcd_display_message_fullscreen_P(MSG_FIND_BED_OFFSET_AND_SKEW_LINE1, next_line);
    if (next_line > 3)
        next_line = 3;
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */

    // Collect the rear 2x3 points.
    current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
    for (int k = 0; k < 4; ++ k) {
        // Don't let the manage_inactivity() function remove power from the motors.
        refresh_cmd_timeout();
#ifdef MESH_BED_CALIBRATION_SHOW_LCD
        lcd_implementation_print_at(0, next_line, k+1);
        lcd_printPGM(MSG_FIND_BED_OFFSET_AND_SKEW_LINE2);
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */
        float *pt = pts + k * 2;
        // Go up to z_initial.
        go_to_current(homing_feedrate[Z_AXIS] / 60.f);
        if (verbosity_level >= 20) {
            // Go to Y0, wait, then go to Y-4.
            current_position[Y_AXIS] = 0.f;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            SERIAL_ECHOLNPGM("At Y0");
            delay_keep_alive(5000);
            current_position[Y_AXIS] = Y_MIN_POS;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            SERIAL_ECHOLNPGM("At Y-4");
            delay_keep_alive(5000);
        }
        // Go to the measurement point position.
        current_position[X_AXIS] = pgm_read_float(bed_ref_points_4+k*2);
        current_position[Y_AXIS] = pgm_read_float(bed_ref_points_4+k*2+1);
        go_to_current(homing_feedrate[X_AXIS] / 60.f);
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
        if (! find_bed_induction_sensor_point_xy())
            return BED_SKEW_OFFSET_DETECTION_POINT_NOT_FOUND;
#if 1
        if (k == 0) {
            // Improve the position of the 1st row sensor points by a zig-zag movement.
            find_bed_induction_sensor_point_z();
            int8_t i = 4;
            for (;;) {
                if (improve_bed_induction_sensor_point3(verbosity_level))
                    break;
                if (-- i == 0)
                    return BED_SKEW_OFFSET_DETECTION_POINT_NOT_FOUND;
                // Try to move the Z axis down a bit to increase a chance of the sensor to trigger.
                current_position[Z_AXIS] -= 0.025f;
                enable_endstops(false);
                enable_z_endstop(false);
                go_to_current(homing_feedrate[Z_AXIS]);
            }
            if (i == 0)
                // not found
                return BED_SKEW_OFFSET_DETECTION_POINT_NOT_FOUND;
        }
#endif
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
        // Save the detected point position and then clamp the Y coordinate, which may have been estimated
        // to lie outside the machine working space.
        pt[0] = current_position[X_AXIS];
        pt[1] = current_position[Y_AXIS];
        if (current_position[Y_AXIS] < Y_MIN_POS)
            current_position[Y_AXIS] = Y_MIN_POS;
        // Start searching for the other points at 3mm above the last point.
        current_position[Z_AXIS] += 3.f;
        cntr[0] += pt[0];
        cntr[1] += pt[1];
        if (verbosity_level >= 10 && k == 0) {
            // Show the zero. Test, whether the Y motor skipped steps.
            current_position[Y_AXIS] = MANUAL_Y_HOME_POS;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            delay_keep_alive(3000);
        }
    }

    if (verbosity_level >= 20) {
        // Test the positions. Are the positions reproducible? Now the calibration is active in the planner.
        delay_keep_alive(3000);
        for (int8_t mesh_point = 0; mesh_point < 4; ++ mesh_point) {
            // Don't let the manage_inactivity() function remove power from the motors.
            refresh_cmd_timeout();
            // Go to the measurement point.
            // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
            current_position[X_AXIS] = pts[mesh_point*2];
            current_position[Y_AXIS] = pts[mesh_point*2+1];
            go_to_current(homing_feedrate[X_AXIS]/60);
            delay_keep_alive(3000);
        }
    }

    BedSkewOffsetDetectionResultType result = calculate_machine_skew_and_offset_LS(pts, 4, bed_ref_points_4, vec_x, vec_y, cntr, verbosity_level);
    if (result >= 0) {
        world2machine_update(vec_x, vec_y, cntr);
    #if 1
        // Fearlessly store the calibration values into the eeprom.
        eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_CENTER+0), cntr [0]);
        eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_CENTER+4), cntr [1]);
        eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +0), vec_x[0]);
        eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +4), vec_x[1]);
        eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +0), vec_y[0]);
        eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +4), vec_y[1]);
    #endif
		if (verbosity_level >= 10) {
			// Length of the vec_x
			float l = sqrt(vec_x[0] * vec_x[0] + vec_x[1] * vec_x[1]);
			SERIAL_ECHOLNPGM("X vector length:");
			MYSERIAL.println(l);

			// Length of the vec_y
			l = sqrt(vec_y[0] * vec_y[0] + vec_y[1] * vec_y[1]);
			SERIAL_ECHOLNPGM("Y vector length:");
			MYSERIAL.println(l);
			// Zero point correction
			l = sqrt(cntr[0] * cntr[0] + cntr[1] * cntr[1]);
			SERIAL_ECHOLNPGM("Zero point correction:");
			MYSERIAL.println(l);

			// vec_x and vec_y shall be nearly perpendicular.
			l = vec_x[0] * vec_y[0] + vec_x[1] * vec_y[1];
			SERIAL_ECHOLNPGM("Perpendicularity");
			MYSERIAL.println(fabs(l));
			SERIAL_ECHOLNPGM("Saving bed calibration vectors to EEPROM");
		}
        // Correct the current_position to match the transformed coordinate system after world2machine_rotation_and_skew and world2machine_shift were set.
        world2machine_update_current();

        if (verbosity_level >= 20) {
            // Test the positions. Are the positions reproducible? Now the calibration is active in the planner.
            delay_keep_alive(3000);
            for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
                // Don't let the manage_inactivity() function remove power from the motors.
                refresh_cmd_timeout();
                // Go to the measurement point.
                // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
                current_position[X_AXIS] = pgm_read_float(bed_ref_points+mesh_point*2);
                current_position[Y_AXIS] = pgm_read_float(bed_ref_points+mesh_point*2+1);
                go_to_current(homing_feedrate[X_AXIS]/60);
                delay_keep_alive(3000);
            }
        }
    }

    return result;
}

BedSkewOffsetDetectionResultType improve_bed_offset_and_skew(int8_t method, int8_t verbosity_level, uint8_t &too_far_mask)
{
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Mask of the first three points. Are they too far?
    too_far_mask = 0;

    // Reusing the z_values memory for the measurement cache.
    // 7x7=49 floats, good for 16 (x,y,z) vectors.
    float *pts = &mbl.z_values[0][0];
    float *vec_x = pts + 2 * 9;
    float *vec_y = vec_x + 2;
    float *cntr  = vec_y + 2;
    memset(pts, 0, sizeof(float) * 7 * 7);

    // Cache the current correction matrix.
    world2machine_initialize();
    vec_x[0] = world2machine_rotation_and_skew[0][0];
    vec_x[1] = world2machine_rotation_and_skew[1][0];
    vec_y[0] = world2machine_rotation_and_skew[0][1];
    vec_y[1] = world2machine_rotation_and_skew[1][1];
    cntr[0] = world2machine_shift[0];
    cntr[1] = world2machine_shift[1];
    // and reset the correction matrix, so the planner will not do anything.
    world2machine_reset();

    bool endstops_enabled  = enable_endstops(false);
    bool endstop_z_enabled = enable_z_endstop(false);

#ifdef MESH_BED_CALIBRATION_SHOW_LCD
    uint8_t next_line;
    lcd_display_message_fullscreen_P(MSG_IMPROVE_BED_OFFSET_AND_SKEW_LINE1, next_line);
    if (next_line > 3)
        next_line = 3;
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */

    // Collect a matrix of 9x9 points.
    BedSkewOffsetDetectionResultType result = BED_SKEW_OFFSET_DETECTION_PERFECT;
    for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
        // Don't let the manage_inactivity() function remove power from the motors.
        refresh_cmd_timeout();
        // Print the decrasing ID of the measurement point.
#ifdef MESH_BED_CALIBRATION_SHOW_LCD
        lcd_implementation_print_at(0, next_line, mesh_point+1);
        lcd_printPGM(MSG_IMPROVE_BED_OFFSET_AND_SKEW_LINE2);
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */

        // Move up.
        current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
        enable_endstops(false);
        enable_z_endstop(false);
        go_to_current(homing_feedrate[Z_AXIS]/60);
        if (verbosity_level >= 20) {
            // Go to Y0, wait, then go to Y-4.
            current_position[Y_AXIS] = 0.f;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            SERIAL_ECHOLNPGM("At Y0");
            delay_keep_alive(5000);
            current_position[Y_AXIS] = Y_MIN_POS;
            go_to_current(homing_feedrate[X_AXIS] / 60.f);
            SERIAL_ECHOLNPGM("At Y-4");
            delay_keep_alive(5000);
        }
        // Go to the measurement point.
        // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
        current_position[X_AXIS] = vec_x[0] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[0] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[0];
        current_position[Y_AXIS] = vec_x[1] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[1] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[1];
        // The calibration points are very close to the min Y.
        if (current_position[Y_AXIS] < Y_MIN_POS_FOR_BED_CALIBRATION)
            current_position[Y_AXIS] = Y_MIN_POS_FOR_BED_CALIBRATION;
        go_to_current(homing_feedrate[X_AXIS]/60);
        // Find its Z position by running the normal vertical search.
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
        find_bed_induction_sensor_point_z();
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
        // Try to move the Z axis down a bit to increase a chance of the sensor to trigger.
        current_position[Z_AXIS] -= 0.025f;
        // Improve the point position by searching its center in a current plane.
        int8_t n_errors = 3;
        for (int8_t iter = 0; iter < 8; ) {
            if (verbosity_level > 20) {
                SERIAL_ECHOPGM("Improving bed point ");
                SERIAL_ECHO(mesh_point);
                SERIAL_ECHOPGM(", iteration ");
                SERIAL_ECHO(iter);
                SERIAL_ECHOPGM(", z");
                MYSERIAL.print(current_position[Z_AXIS], 5);
                SERIAL_ECHOLNPGM("");
            }
            bool found = false;
            if (mesh_point < 3) {
                // Because the sensor cannot move in front of the first row
                // of the sensor points, the y position cannot be measured
                // by a cross center method.
                // Use a zig-zag search for the first row of the points.
                found = improve_bed_induction_sensor_point3(verbosity_level);
            } else {
                switch (method) {
                    case 0: found = improve_bed_induction_sensor_point(); break;
                    case 1: found = improve_bed_induction_sensor_point2(mesh_point < 3, verbosity_level); break;
                    default: break;
                }
            }
            if (found) {
                if (iter > 3) {
                    // Average the last 4 measurements.
                    pts[mesh_point*2  ] += current_position[X_AXIS];
                    pts[mesh_point*2+1] += current_position[Y_AXIS];
                }
                if (current_position[Y_AXIS] < Y_MIN_POS)
                    current_position[Y_AXIS] = Y_MIN_POS;
                ++ iter;
            } else if (n_errors -- == 0) {
                // Give up.
                result = BED_SKEW_OFFSET_DETECTION_POINT_NOT_FOUND;
                goto canceled;
            } else {
                // Try to move the Z axis down a bit to increase a chance of the sensor to trigger.
                current_position[Z_AXIS] -= 0.05f;
                enable_endstops(false);
                enable_z_endstop(false);
                go_to_current(homing_feedrate[Z_AXIS]);
                if (verbosity_level >= 5) {
                    SERIAL_ECHOPGM("Improving bed point ");
                    SERIAL_ECHO(mesh_point);
                    SERIAL_ECHOPGM(", iteration ");
                    SERIAL_ECHO(iter);
                    SERIAL_ECHOPGM(" failed. Lowering z to ");
                    MYSERIAL.print(current_position[Z_AXIS], 5);
                    SERIAL_ECHOLNPGM("");
                }
            }
        }
        if (verbosity_level >= 10)
            delay_keep_alive(3000);
    }
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Average the last 4 measurements.
    for (int8_t i = 0; i < 18; ++ i)
        pts[i] *= (1.f/4.f);

    enable_endstops(false);
    enable_z_endstop(false);

    if (verbosity_level >= 5) {
        // Test the positions. Are the positions reproducible?
        for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
            // Don't let the manage_inactivity() function remove power from the motors.
            refresh_cmd_timeout();
            // Go to the measurement point.
            // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
            current_position[X_AXIS] = pts[mesh_point*2];
            current_position[Y_AXIS] = pts[mesh_point*2+1];
            if (verbosity_level >= 10) {
                go_to_current(homing_feedrate[X_AXIS]/60);
                delay_keep_alive(3000);
            }
            SERIAL_ECHOPGM("Final measured bed point ");
            SERIAL_ECHO(mesh_point);
            SERIAL_ECHOPGM(": ");
            MYSERIAL.print(current_position[X_AXIS], 5);
            SERIAL_ECHOPGM(", ");
            MYSERIAL.print(current_position[Y_AXIS], 5);
            SERIAL_ECHOLNPGM("");
        }
    }

    {
        // First fill in the too_far_mask from the measured points.
        for (uint8_t mesh_point = 0; mesh_point < 3; ++ mesh_point)
            if (pts[mesh_point * 2 + 1] < Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH)
                too_far_mask |= 1 << mesh_point;
        result = calculate_machine_skew_and_offset_LS(pts, 9, bed_ref_points, vec_x, vec_y, cntr, verbosity_level);
        if (result < 0) {
            SERIAL_ECHOLNPGM("Calculation of the machine skew and offset failed.");
            goto canceled;
        }
        // In case of success, update the too_far_mask from the calculated points.
        for (uint8_t mesh_point = 0; mesh_point < 3; ++ mesh_point) {
            float y = vec_x[1] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[1] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[1];
            if (y < Y_MIN_POS_CALIBRATION_POINT_OUT_OF_REACH)
                too_far_mask |= 1 << mesh_point;
        }
    }

    world2machine_update(vec_x, vec_y, cntr);
#if 1
    // Fearlessly store the calibration values into the eeprom.
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_CENTER+0), cntr [0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_CENTER+4), cntr [1]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +0), vec_x[0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_X +4), vec_x[1]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +0), vec_y[0]);
    eeprom_update_float((float*)(EEPROM_BED_CALIBRATION_VEC_Y +4), vec_y[1]);
#endif

    // Correct the current_position to match the transformed coordinate system after world2machine_rotation_and_skew and world2machine_shift were set.
    world2machine_update_current();

    enable_endstops(false);
    enable_z_endstop(false);

    if (verbosity_level >= 5) {
        // Test the positions. Are the positions reproducible? Now the calibration is active in the planner.
        delay_keep_alive(3000);
        for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
            // Don't let the manage_inactivity() function remove power from the motors.
            refresh_cmd_timeout();
            // Go to the measurement point.
            // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
            current_position[X_AXIS] = pgm_read_float(bed_ref_points+mesh_point*2);
            current_position[Y_AXIS] = pgm_read_float(bed_ref_points+mesh_point*2+1);
            if (verbosity_level >= 10) {
                go_to_current(homing_feedrate[X_AXIS]/60);
                delay_keep_alive(3000);
            }
            {
                float x, y;
                world2machine(current_position[X_AXIS], current_position[Y_AXIS], x, y);
                SERIAL_ECHOPGM("Final calculated bed point ");
                SERIAL_ECHO(mesh_point);
                SERIAL_ECHOPGM(": ");
                MYSERIAL.print(x, 5);
                SERIAL_ECHOPGM(", ");
                MYSERIAL.print(y, 5);
                SERIAL_ECHOLNPGM("");
            }
        }
    }

    // Sample Z heights for the mesh bed leveling.
    // In addition, store the results into an eeprom, to be used later for verification of the bed leveling process.
    if (! sample_mesh_and_store_reference())
        goto canceled;

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();
    return result;

canceled:
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();
    // Print head up.
    current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
    go_to_current(homing_feedrate[Z_AXIS]/60);
    // Store the identity matrix to EEPROM.
    reset_bed_offset_and_skew();
    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return result;
}

void go_home_with_z_lift()
{
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();
    // Go home.
    // First move up to a safe height.
    current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
    go_to_current(homing_feedrate[Z_AXIS]/60);
    // Second move to XY [0, 0].
    current_position[X_AXIS] = X_MIN_POS+0.2;
    current_position[Y_AXIS] = Y_MIN_POS+0.2;
    // Clamp to the physical coordinates.
    world2machine_clamp(current_position[X_AXIS], current_position[Y_AXIS]);
    go_to_current(homing_feedrate[X_AXIS]/60);
    // Third move up to a safe height.
    current_position[Z_AXIS] = Z_MIN_POS;
    go_to_current(homing_feedrate[Z_AXIS]/60);    
}

// Sample the 9 points of the bed and store them into the EEPROM as a reference.
// When calling this function, the X, Y, Z axes should be already homed,
// and the world2machine correction matrix should be active.
// Returns false if the reference values are more than 3mm far away.
bool sample_mesh_and_store_reference()
{
    bool endstops_enabled  = enable_endstops(false);
    bool endstop_z_enabled = enable_z_endstop(false);

    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

#ifdef MESH_BED_CALIBRATION_SHOW_LCD
    uint8_t next_line;
    lcd_display_message_fullscreen_P(MSG_MEASURE_BED_REFERENCE_HEIGHT_LINE1, next_line);
    if (next_line > 3)
        next_line = 3;
    // display "point xx of yy"
    lcd_implementation_print_at(0, next_line, 1);
    lcd_printPGM(MSG_MEASURE_BED_REFERENCE_HEIGHT_LINE2);
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */

    // Sample Z heights for the mesh bed leveling.
    // In addition, store the results into an eeprom, to be used later for verification of the bed leveling process.
    {
        // The first point defines the reference.
        current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
        go_to_current(homing_feedrate[Z_AXIS]/60);
        current_position[X_AXIS] = pgm_read_float(bed_ref_points);
        current_position[Y_AXIS] = pgm_read_float(bed_ref_points+1);
        world2machine_clamp(current_position[X_AXIS], current_position[Y_AXIS]);
        go_to_current(homing_feedrate[X_AXIS]/60);
        memcpy(destination, current_position, sizeof(destination));
        enable_endstops(true);
        homeaxis(Z_AXIS);
        enable_endstops(false);
        find_bed_induction_sensor_point_z();
        mbl.set_z(0, 0, current_position[Z_AXIS]);
    }
    for (int8_t mesh_point = 1; mesh_point != MESH_MEAS_NUM_X_POINTS * MESH_MEAS_NUM_Y_POINTS; ++ mesh_point) {
        // Don't let the manage_inactivity() function remove power from the motors.
        refresh_cmd_timeout();
        // Print the decrasing ID of the measurement point.
        current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
        go_to_current(homing_feedrate[Z_AXIS]/60);
        current_position[X_AXIS] = pgm_read_float(bed_ref_points+2*mesh_point);
        current_position[Y_AXIS] = pgm_read_float(bed_ref_points+2*mesh_point+1);
        world2machine_clamp(current_position[X_AXIS], current_position[Y_AXIS]);
        go_to_current(homing_feedrate[X_AXIS]/60);
#ifdef MESH_BED_CALIBRATION_SHOW_LCD
        // display "point xx of yy"
        lcd_implementation_print_at(0, next_line, mesh_point+1);
        lcd_printPGM(MSG_MEASURE_BED_REFERENCE_HEIGHT_LINE2);
#endif /* MESH_BED_CALIBRATION_SHOW_LCD */
        find_bed_induction_sensor_point_z();
        // Get cords of measuring point
        int8_t ix = mesh_point % MESH_MEAS_NUM_X_POINTS;
        int8_t iy = mesh_point / MESH_MEAS_NUM_X_POINTS;
        if (iy & 1) ix = (MESH_MEAS_NUM_X_POINTS - 1) - ix; // Zig zag
        mbl.set_z(ix, iy, current_position[Z_AXIS]);
    }
    {
        // Verify the span of the Z values.
        float zmin = mbl.z_values[0][0];
        float zmax = zmax;
        for (int8_t j = 0; j < 3; ++ j)
           for (int8_t i = 0; i < 3; ++ i) {
                zmin = min(zmin, mbl.z_values[j][i]);
                zmax = min(zmax, mbl.z_values[j][i]);
           }
        if (zmax - zmin > 3.f) {
            // The span of the Z offsets is extreme. Give up.
            // Homing failed on some of the points.
            SERIAL_PROTOCOLLNPGM("Exreme span of the Z values!");
            return false;
        }
    }

    // Store the correction values to EEPROM.
    // Offsets of the Z heiths of the calibration points from the first point.
    // The offsets are saved as 16bit signed int, scaled to tenths of microns.
    {
        uint16_t addr = EEPROM_BED_CALIBRATION_Z_JITTER;
        for (int8_t j = 0; j < 3; ++ j)
            for (int8_t i = 0; i < 3; ++ i) {
                if (i == 0 && j == 0)
                    continue;
                float dif = mbl.z_values[j][i] - mbl.z_values[0][0];
                int16_t dif_quantized = int16_t(floor(dif * 100.f + 0.5f));
                eeprom_update_word((uint16_t*)addr, *reinterpret_cast<uint16_t*>(&dif_quantized));
                #if 0
                {
                    uint16_t z_offset_u = eeprom_read_word((uint16_t*)addr);
                    float dif2 = *reinterpret_cast<int16_t*>(&z_offset_u) * 0.01;

                    SERIAL_ECHOPGM("Bed point ");
                    SERIAL_ECHO(i);
                    SERIAL_ECHOPGM(",");
                    SERIAL_ECHO(j);
                    SERIAL_ECHOPGM(", differences: written ");
                    MYSERIAL.print(dif, 5);
                    SERIAL_ECHOPGM(", read: ");
                    MYSERIAL.print(dif2, 5);
                    SERIAL_ECHOLNPGM("");
                }
                #endif
                addr += 2;
            }
    }

    mbl.upsample_3x3();
    mbl.active = true;

    go_home_with_z_lift();

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return true;
}

bool scan_bed_induction_points(int8_t verbosity_level)
{
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    // Reusing the z_values memory for the measurement cache.
    // 7x7=49 floats, good for 16 (x,y,z) vectors.
    float *pts = &mbl.z_values[0][0];
    float *vec_x = pts + 2 * 9;
    float *vec_y = vec_x + 2;
    float *cntr  = vec_y + 2;
    memset(pts, 0, sizeof(float) * 7 * 7);

    // Cache the current correction matrix.
    world2machine_initialize();
    vec_x[0] = world2machine_rotation_and_skew[0][0];
    vec_x[1] = world2machine_rotation_and_skew[1][0];
    vec_y[0] = world2machine_rotation_and_skew[0][1];
    vec_y[1] = world2machine_rotation_and_skew[1][1];
    cntr[0] = world2machine_shift[0];
    cntr[1] = world2machine_shift[1];
    // and reset the correction matrix, so the planner will not do anything.
    world2machine_reset();

    bool endstops_enabled  = enable_endstops(false);
    bool endstop_z_enabled = enable_z_endstop(false);

    // Collect a matrix of 9x9 points.
    for (int8_t mesh_point = 0; mesh_point < 9; ++ mesh_point) {
        // Don't let the manage_inactivity() function remove power from the motors.
        refresh_cmd_timeout();

        // Move up.
        current_position[Z_AXIS] = MESH_HOME_Z_SEARCH;
        enable_endstops(false);
        enable_z_endstop(false);
        go_to_current(homing_feedrate[Z_AXIS]/60);
        // Go to the measurement point.
        // Use the coorrected coordinate, which is a result of find_bed_offset_and_skew().
        current_position[X_AXIS] = vec_x[0] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[0] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[0];
        current_position[Y_AXIS] = vec_x[1] * pgm_read_float(bed_ref_points+mesh_point*2) + vec_y[1] * pgm_read_float(bed_ref_points+mesh_point*2+1) + cntr[1];
        // The calibration points are very close to the min Y.
        if (current_position[Y_AXIS] < Y_MIN_POS_FOR_BED_CALIBRATION)
            current_position[Y_AXIS] = Y_MIN_POS_FOR_BED_CALIBRATION;
        go_to_current(homing_feedrate[X_AXIS]/60);
        find_bed_induction_sensor_point_z();
        scan_bed_induction_sensor_point();
    }
    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    enable_endstops(false);
    enable_z_endstop(false);

    // Don't let the manage_inactivity() function remove power from the motors.
    refresh_cmd_timeout();

    enable_endstops(endstops_enabled);
    enable_z_endstop(endstop_z_enabled);
    return true;
}

// Shift a Z axis by a given delta.
// To replace loading of the babystep correction.
static void shift_z(float delta)
{
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS] - delta, current_position[E_AXIS], homing_feedrate[Z_AXIS]/40, active_extruder);
    st_synchronize();
    plan_set_z_position(current_position[Z_AXIS]);
}

#define BABYSTEP_LOADZ_BY_PLANNER

// Number of baby steps applied
static int babystepLoadZ = 0;

void babystep_apply()
{
    // Apply Z height correction aka baby stepping before mesh bed leveling gets activated.
    if(calibration_status() == CALIBRATION_STATUS_CALIBRATED)
    {
		check_babystep(); //checking if babystep is in allowed range, otherwise setting babystep to 0
		
		// End of G80: Apply the baby stepping value.
        EEPROM_read_B(EEPROM_BABYSTEP_Z,&babystepLoadZ);
							
    #if 0
        SERIAL_ECHO("Z baby step: ");
        SERIAL_ECHO(babystepLoadZ);
        SERIAL_ECHO(", current Z: ");
        SERIAL_ECHO(current_position[Z_AXIS]);
        SERIAL_ECHO("correction: ");
        SERIAL_ECHO(float(babystepLoadZ) / float(axis_steps_per_unit[Z_AXIS]));
        SERIAL_ECHOLN("");
    #endif
    #ifdef BABYSTEP_LOADZ_BY_PLANNER
        shift_z(- float(babystepLoadZ) / float(axis_steps_per_unit[Z_AXIS]));
    #else
        babystepsTodoZadd(babystepLoadZ);
    #endif /* BABYSTEP_LOADZ_BY_PLANNER */
    }
}

void babystep_undo()
{
#ifdef BABYSTEP_LOADZ_BY_PLANNER
      shift_z(float(babystepLoadZ) / float(axis_steps_per_unit[Z_AXIS]));
#else
      babystepsTodoZsubtract(babystepLoadZ);
#endif /* BABYSTEP_LOADZ_BY_PLANNER */
      babystepLoadZ = 0;
}

void babystep_reset()
{
      babystepLoadZ = 0;    
}