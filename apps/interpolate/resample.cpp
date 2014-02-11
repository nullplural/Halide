#include "Halide.h"

using namespace Halide;

#include <image_io.h>

#include <iostream>
#include <limits>

#include <sys/time.h>

double now() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    static bool first_call = true;
    static time_t first_sec = 0;
    if (first_call) {
        first_call = false;
        first_sec = tv.tv_sec;
    }
    assert(tv.tv_sec >= first_sec);
    return (tv.tv_sec - first_sec) + (tv.tv_usec / 1000000.0);
}

enum InterpolationType {
    BOX, LINEAR, CUBIC
};

Expr kernel_box(Expr x) {
    Expr xx = abs(x);
    return select(xx <= 0.5f, 1.0f, 0.0f);
}

Expr kernel_linear(Expr x) {
    Expr xx = abs(x);
    return select(xx < 1.0f, xx, 0.0f);
}

Expr kernel_cubic(Expr x) {
    Expr xx = abs(x);
    Expr xx2 = xx * xx;
    Expr xx3 = xx2 * xx;
    float a = -0.5f;

    return select(xx < 1.0f, (a + 2.0f) * xx3 + (a + 3.0f) * xx2 + 1,
                  select (xx < 2.0f, a * xx3 - 5 * a * xx2 + 8 * a * xx - 4.0f * a,
                          0.0f));
}

struct KernelInfo {
    const char *name;
    float size;
    Expr (*kernel)(Expr);
};

static KernelInfo kernelInfo[] = {
    { "box", 0.5f, kernel_box },
    { "linear", 1.0f, kernel_linear },
    { "cubic", 2.0f, kernel_cubic }
};


std::string infile, outfile;
InterpolationType interpolationType = LINEAR;
float scaleFactor = 1.0f;
bool show_usage = false;
int schedule = 0;

void parse_commandline(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-f" && i+1 < argc) {
            scaleFactor = atof(argv[++i]);
        } else if (arg == "-s" && i+1 < argc) {
            schedule = atoi(argv[++i]);
            if (schedule < 0 || schedule > 3) {
                fprintf(stderr, "Invalid schedule\n");
                show_usage = true;
            }
        } else if (arg == "-t" && i+1 < argc) {
            arg = argv[++i];
            if (arg == "box") {
                interpolationType = BOX;
            } else if (arg == "linear") {
                interpolationType = LINEAR;
            } else if (arg == "cubic") {
                interpolationType = CUBIC;
            } else {
                fprintf(stderr, "Invalid interpolation type '%s' specified.\n",
                        arg.c_str());
                show_usage = true;
            }
        } else if (infile.empty()) {
            infile = arg;
        } else if (outfile.empty()) {
            outfile = arg;
        } else {
            fprintf(stderr, "Unexpected command line option '%s'.\n", arg.c_str());
        }
    }
}

int main(int argc, char **argv) {
    parse_commandline(argc, argv);
    if (infile.empty() || outfile.empty() || show_usage) {
        fprintf(stderr,
                "Usage:\n"
                "\t./resample [-f scalefactor] [-s schedule] [-t box|linear|cubic] in.png out.png\n"
                "\t\tSchedules: 0=default 1=vectorized 2=parallel 3=vectorized+parallel\n");
        return 1;
    }

    ImageParam input(Float(32), 3);

    Var x("x"), y("y"), c("c"), k("k");

    Func clamped("clamped");
    clamped(x, y, c) = input(clamp(x, 0, input.width()-1), clamp(y, 0, input.height()-1), c);

    // For downscaling, widen the interpolation kernel to perform lowpass
    // filtering.
    float kernelScaling = std::min(scaleFactor, 1.0f);
    float kernelSize = kernelInfo[interpolationType].size / kernelScaling;

    // source[xy] are the (non-integer) coordinates inside the source image
    Expr sourcex = (x + 0.5f) / scaleFactor;
    Expr sourcey = (y + 0.5f) / scaleFactor;

    // Initialize interpolation kernels. Since we allow an arbitrary
    // scaling factor, the filter coefficients are different for each x
    // and y coordinate.
    Func kernelx("kernelx"), kernely("kernely");
    Expr beginx = cast<int>(sourcex - kernelSize + 0.5f);
    Expr beginy = cast<int>(sourcey - kernelSize + 0.5f);
    RDom domx(0, static_cast<int>(2.0f*kernelSize)+1, "domx");
    RDom domy(0, static_cast<int>(2.0f*kernelSize)+1, "domy");
    {
        const KernelInfo &info = kernelInfo[interpolationType];
        Func kx, ky;
        kx(x, k) = info.kernel((k + beginx - sourcex) * kernelScaling);
        ky(y, k) = info.kernel((k + beginy - sourcey) * kernelScaling);
        kernelx(x, k) = kx(x, k) / sum(kx(x, domx));
        kernely(y, k) = ky(y, k) / sum(ky(y, domy));
    }

    // Perform separable resizing
    Func resized_x("resized_x");
    Func resized_y("resized_y");
    resized_x(x, y, c) = sum(kernelx(x, domx) * cast<float>(clamped(domx + beginx, y, c)));
    resized_y(x, y, c) = sum(kernely(y, domy) * resized_x(x, domy + beginy, c));

    Func final("final");
    final(x, y, c) = clamp(resized_y(x, y, c), 0.0f, 1.0f);

    std::cout << "Finished function setup." << std::endl;

    // Scheduling
    bool parallelize = (schedule >= 2);
    bool vectorize = (schedule == 1 || schedule == 3);

    kernelx.compute_root();
    kernely.compute_at(final, y);

    if (vectorize) {
        resized_x.vectorize(x, 4);
        final.vectorize(x, 4);
    }

    if (parallelize) {
        resized_x.compute_root();
        resized_x.parallel(y);
        final.parallel(y);
    } else {
        // resized_x.store_root().compute_at(final, y);
        resized_x.compute_root();
    }

    Target target = get_jit_target_from_environment();
    final.compile_jit(target);

    printf("Loading '%s'\n", infile.c_str());
    Image<float> in_png = load<float>(infile);
    int out_width = in_png.width() * scaleFactor;
    int out_height = in_png.height() * scaleFactor;
    Image<float> out(out_width, out_height, 3);
    input.set(in_png);
    printf("Resampling '%s' from %dx%d to %dx%d using %s interpolation\n",
           infile.c_str(),
           in_png.width(), in_png.height(),
           out_width, out_height,
           kernelInfo[interpolationType].name);

    double min = std::numeric_limits<double>::infinity();
    const unsigned int iters = 20;

    for (unsigned int x = 0; x < iters; ++x) {
        double before = now();
        final.realize(out);
        double after = now();
        double amt = after - before;

        std::cout << "   " << amt * 1000 << std::endl;
        if (amt < min) min = amt;

    }
    std::cout << " took " << min * 1000 << " msec." << std::endl;

    save(out, outfile);
}
