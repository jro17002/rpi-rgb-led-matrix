// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// Example how to display an image, including animated images using
// ImageMagick. For a full utility that does a few more things, have a look
// at the led-image-viewer in ../utils
//
// Showing an image is not so complicated, essentially just copy all the
// pixels to the canvas. How to get the pixels ? In this example we're using
// the graphicsmagick library as universal image loader library that
// can also deal with animated images.
// You can of course do your own image loading or use some other library.
//
// This requires an external dependency, so install these first before you
// can call `make image-example`
//   sudo apt-get update
//   sudo apt-get install libgraphicsmagick++-dev libwebp-dev -y
//   make image-example

#include "led-matrix.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <exception>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <Magick++.h>
#include <magick/image.h>

using rgb_matrix::Canvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;

// Make sure we can exit gracefully when Ctrl-C is pressed.
volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}

using ImageVector = std::vector<Magick::Image>;

static std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  return value;
}

static bool HasImageExtension(const std::string &filename) {
  const std::string lower = ToLower(filename);
  return lower.size() >= 4 &&
         (lower.substr(lower.size() - 4) == ".png" ||
          lower.substr(lower.size() - 4) == ".jpg" ||
          lower.substr(lower.size() - 5) == ".jpeg" ||
          lower.substr(lower.size() - 4) == ".gif" ||
          lower.substr(lower.size() - 4) == ".bmp");
}

static std::vector<std::string> ListImageFilesInDirectory(const std::string &dir) {
  std::vector<std::string> files;
  DIR *handle = opendir(dir.c_str());
  if (!handle) {
    fprintf(stderr, "Failed to open directory '%s'\n", dir.c_str());
    return files;
  }

  while (struct dirent *entry = readdir(handle)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;

    const std::string full_path = dir + "/" + name;
    if (HasImageExtension(name))
      files.push_back(full_path);
  }
  closedir(handle);

  std::sort(files.begin(), files.end());
  return files;
}

// Given the filename, load the image and scale to the size of the
// matrix. If this is an animated image, the resulting vector will contain
// multiple frames. For a directory, each image in the directory becomes one
// frame in the returned list.
static ImageVector LoadImageAndScaleImage(const char *filename,
                                          int target_width,
                                          int target_height) {
  ImageVector result;

  struct stat st;
  if (stat(filename, &st) != 0) {
    fprintf(stderr, "Unable to access '%s'\n", filename);
    return result;
  }

  if (S_ISDIR(st.st_mode)) {
    const std::vector<std::string> files = ListImageFilesInDirectory(filename);
    for (const std::string &path : files) {
      Magick::Image image(path);
      image.scale(Magick::Geometry(target_width, target_height));
      result.push_back(image);
    }
    return result;
  }

  ImageVector frames;
  try {
    readImages(&frames, filename);
  } catch (std::exception &e) {
    if (e.what())
      fprintf(stderr, "%s\n", e.what());
    return result;
  }

  if (frames.empty()) {
    fprintf(stderr, "No image found.\n");
    return result;
  }

  // Animated images have partial frames that need to be put together.
  if (frames.size() > 1) {
    Magick::coalesceImages(&result, frames.begin(), frames.end());
  } else {
    result.push_back(frames[0]);
  }

  for (Magick::Image &image : result) {
    image.scale(Magick::Geometry(target_width, target_height));
  }

  return result;
}

// Copy an image to a Canvas. Note, the RGBMatrix is implementing the Canvas
// interface as well as the FrameCanvas we use in the double-buffering of the
// animated image.
void CopyImageToCanvas(const Magick::Image &image, Canvas *canvas) {
  const int src_w = image.columns();
  const int src_h = image.rows();

  for (int y = 0; y < src_h; ++y) {
    for (int x = 0; x < src_w; ++x) {
      const Magick::Color &c = image.pixelColor(x, y);
      if (c.alphaQuantum() < 256) {
        const int red = ScaleQuantumToChar(c.redQuantum());
        const int green = ScaleQuantumToChar(c.greenQuantum());
        const int blue = ScaleQuantumToChar(c.blueQuantum());

        if ((red | green | blue) == 0) {
          continue;  // Skip black pixels: they do not need to be lit.
        }

        // Rotate the source image 90 degrees counter-clockwise to fit the
        // 64x32 portrait matrix layout.
        const int dst_x = src_h - 1 - y;
        const int dst_y = x;
        canvas->SetPixel(dst_x, dst_y, red, green, blue);
      }
    }
  }
}

// An animated image has to constantly swap to the next frame.
// We're using double-buffering and fill an offscreen buffer first, then show.
void ShowAnimatedImage(const ImageVector &images, RGBMatrix *matrix) {
  FrameCanvas *offscreen_canvas = matrix->CreateFrameCanvas();
  const int kFrameDelayUs = 1000000;  // 1 FPS

  while (!interrupt_received) {
    for (const auto &image : images) {
      if (interrupt_received) break;
      offscreen_canvas->Clear();
      CopyImageToCanvas(image, offscreen_canvas);
      offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas);
      usleep(kFrameDelayUs);
    }
  }
}

int usage(const char *progname) {
  fprintf(stderr,
          "Usage: %s [led-matrix-options] <image-file-or-directory>\n",
          progname);
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  Magick::InitializeMagick(*argv);

  // Default to a 32x64 matrix for the Raspberry Pi board in this project.
  RGBMatrix::Options matrix_options;
  matrix_options.rows = 64;
  matrix_options.cols = 32;

  rgb_matrix::RuntimeOptions runtime_opt;
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  if (argc != 2)
    return usage(argv[0]);
  const char *filename = argv[1];

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL)
    return 1;

  ImageVector images = LoadImageAndScaleImage(filename,
                                              matrix->width(),
                                              matrix->height());
  switch (images.size()) {
  case 0:   // failed to load image.
    break;
  case 1:   // Simple example: one image to show.
    CopyImageToCanvas(images[0], matrix);
    while (!interrupt_received) sleep(1000);  // Until Ctrl-C is pressed.
    break;
  default:  // More than one image: this is an animation.
    ShowAnimatedImage(images, matrix);
    break;
  }

  matrix->Clear();
  delete matrix;

  return 0;
}
