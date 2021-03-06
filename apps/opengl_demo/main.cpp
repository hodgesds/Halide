#include <iostream>
#include <stdlib.h>

#include "png_helpers.h"
#include "glfw_helpers.h"
#include "opengl_helpers.h"
#include "layout.h"
#include "timer.h"

#include <HalideRuntimeOpenGL.h>
#include "sample_filter_cpu.h"
#include "sample_filter_opengl.h"


/*
 * Initializes a halide buffer_t object for 8-bit RGBA data stored
 * interleaved as rgbargba... in row-major order.
 */
buffer_t create_buffer(int width, int height)
{
    const int channels = 4;
    const int elem_size = 1;
    buffer_t buf = {0};
    buf.stride[0] = channels;
    buf.stride[1] = channels * width;
    buf.stride[2] = 1;
    buf.elem_size = elem_size;
    buf.extent[0] = width;
    buf.extent[1] = height;
    buf.extent[2] = channels;
    // buf.host is null by initialization
    // buf.host_dirty is false by initialization
    return buf;
}

/*
 * Runs the filter on the CPU.  Takes a pointer to memory with the image
 * data to filter, and a pointer to memory in which to place the result
 * data.
 */
std::string run_cpu_filter(const uint8_t *image_data, uint8_t *result_data, int width, int height)
{
    const auto time = Timer::start("CPU");

    // Create halide input buffer and point it at the passed image data
    auto input_buf = create_buffer(width, height);
    input_buf.host = (uint8_t *) image_data; // OK to break the const, since we know halide won't change the input

    // Create halide output buffer and point it at the passed result data storage
    auto output_buf = create_buffer(width, height);
    output_buf.host = result_data;

    // Run the AOT-compiled OpenGL filter
    sample_filter_cpu(&input_buf, &output_buf);

    return Timer::report(time);
}

/*
 * Runs the filter on OpenGL.  Takes a pointer to memory with the image
 * data to filter, and a pointer to memory in which to place the result
 * data.
 */
std::string run_opengl_filter_from_host_to_host(const uint8_t *image_data, uint8_t *result_data, int width, int height)
{
    const auto time = Timer::start("OpenGL host-to-host");

    // Create halide input buffer and point it at the passed image data for
    // the host memory.  Halide will automatically allocate a texture to
    // hold the data on the GPU.  Mark the host memory as "dirty" so halide
    // will know it needs to transfer the data to the GPU texture.
    auto input_buf = create_buffer(width, height);
    input_buf.host = (uint8_t *) image_data; // OK to break the const, since we know halide won't change the input
    input_buf.host_dirty = true;

    // Create halide output buffer and point it at the passed result data
    // memory.  Halide will automatically allocate a texture to hold the
    // data on the GPU.
    auto output_buf = create_buffer(width, height);
    output_buf.host = result_data;

    // Run the AOT-compiled OpenGL filter
    sample_filter_opengl(&input_buf, &output_buf);
    halide_copy_to_host(nullptr, &output_buf); // Ensure that halide copies the data back to the host

    return Timer::report(time);
}

/*
 * Runs the filter on OpenGL.  Assumes the data is already in a texture,
 * and leaves the output in a texture
 */
std::string run_opengl_filter_from_texture_to_texture(GLuint input_texture_id, GLuint output_texture_id, int width, int height)
{
    const auto time = Timer::start("OpenGL texture-to-texture");

    // Create halide input buffer and tell it to use the existing GPU
    // texture.  No need to allocate memory on the host since this simple
    // pipeline will run entirely on the GPU.
    auto input_buf = create_buffer(width, height);
    halide_opengl_wrap_texture(nullptr, &input_buf, input_texture_id);

    // Create halide output buffer and tell it to use the existing GPU texture.
    // No need to allocate memory on the host since this simple pipeline will run
    // entirely on the GPU.
    auto output_buf = create_buffer(width, height);
    halide_opengl_wrap_texture(nullptr, &output_buf, output_texture_id);

    // Run the AOT-compiled OpenGL filter
    sample_filter_opengl(&input_buf, &output_buf);

    // Tell halide we are finished using the textures
    halide_opengl_detach_texture(nullptr, &output_buf);
    halide_opengl_detach_texture(nullptr, &input_buf);

    return Timer::report(time);
}

int main(const int argc, const char *argv[])
{
    if (argc != 2) {
	std::cerr << "Usage: " << argv[0] << " filename" << std::endl;
	exit(1);
    }
    const std::string filename = argv[1];

    const auto image = PNGHelpers::load(filename);
    const auto width = image.width;
    const auto height = image.height;


    const auto layout = Layout::setup(width, height);
    const auto glfw = GlfwHelpers::setup(layout.window_width, layout.window_height);
    OpenGLHelpers::setup(glfw.dpi_scale);

    /*
     * Draw the original image
     */
    Layout::draw_image(Layout::UL, image.data, width, height, "Input");

    std::string report;

    /*
     * Draw the result of running the filter on the CPU
     */
    const auto cpu_result_data = (uint8_t *) calloc(width * height * 4, sizeof(uint8_t));
    report = run_cpu_filter(image.data, cpu_result_data, width, height);
    Layout::draw_image(Layout::UR, cpu_result_data, width, height, report);
    free((void*) cpu_result_data);

    /*
     * Draw the result of running the filter on OpenGL, with data starting
     * from and ending up on the host
     */
    const auto opengl_result_data = (uint8_t *) calloc(width * height * 4, sizeof(uint8_t));
    report = run_opengl_filter_from_host_to_host(image.data, opengl_result_data, width, height);
    Layout::draw_image(Layout::LL, opengl_result_data, width, height, report);
    free((void*) opengl_result_data);

    /*
     * Draw the result of running the filter on OpenGL, with data starting
     * from and ending up in a texture on the device
     */
    const auto image_texture_id = OpenGLHelpers::create_texture(width, height, image.data);
    const auto result_texture_id = OpenGLHelpers::create_texture(width, height, nullptr);
    report = run_opengl_filter_from_texture_to_texture(image_texture_id, result_texture_id, width, height);
    Layout::draw_texture(Layout::LR, result_texture_id, width, height, report);
    OpenGLHelpers::delete_texture(image_texture_id);
    OpenGLHelpers::delete_texture(result_texture_id);

    // Release all Halide internal structures for the OpenGL context
    halide_opengl_context_lost(nullptr);

    GlfwHelpers::terminate();

    free((void*) image.data);

    return 0;
}

/*
 * Global definition required by halide with OpenGL backend, to prevent
 * Halide from allocating its own OpenGL context.
 *
 * In general, this function needs to set an active OpenGL context
 * and return 0 on success.
 */

int halide_opengl_create_context(void * /*user_context*/)
{
    GlfwHelpers::set_opengl_context();
    return 0;
}
