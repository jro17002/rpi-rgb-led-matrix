// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-

#include "led-matrix.h"

#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <Magick++.h>

using rgb_matrix::Canvas;
using rgb_matrix::FrameCanvas;
using rgb_matrix::RGBMatrix;

volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}

static bool IsPng(const std::string &path) {
  if (path.size() < 4) return false;
  return path.compare(path.size() - 4, 4, ".png") == 0 ||
         path.compare(path.size() - 4, 4, ".PNG") == 0;
}

static std::vector<std::string> ListPngFiles(const std::string &path) {
  std::vector<std::string> files;
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    fprintf(stderr, "Unable to access '%s'\n", path.c_str());
    return files;
  }

  if (S_ISREG(st.st_mode)) {
    if (IsPng(path)) files.push_back(path);
    return files;
  }

  if (!S_ISDIR(st.st_mode)) {
    fprintf(stderr, "Not a PNG file or directory: '%s'\n", path.c_str());
    return files;
  }

  DIR *dir = opendir(path.c_str());
  if (!dir) {
    fprintf(stderr, "Failed to open directory '%s'\n", path.c_str());
    return files;
  }

  while (struct dirent *entry = readdir(dir)) {
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;

    const std::string full_path = path + "/" + name;
    if (IsPng(full_path)) files.push_back(full_path);
  }
  closedir(dir);

  std::sort(files.begin(), files.end());
  return files;
}

static Magick::Image PrepareImageForMatrix(const Magick::Image &image,
                                          const RGBMatrix *matrix,
                                          int *x_offset) {
  Magick::Image prepared = image;
  *x_offset = 0;

  const bool image_is_portrait = prepared.rows() > prepared.columns();
  const bool matrix_is_landscape = matrix->width() > matrix->height();

  // Keep tall PNGs aligned with the matrix by rotating them to the display
  // orientation before scaling. The 256x640 images are portrait, while the
  // matrix is typically landscape at 64x32, so rotating once fixes the
  // sideways rendering. Apply a small negative X translation to nudge the
  // rotated result left by 3 pixels.
  if (image_is_portrait && matrix_is_landscape) {
    prepared.rotate(270.0);
    *x_offset = -3;
  } else if (!image_is_portrait && !matrix_is_landscape) {
    prepared.rotate(-270.0);
  }

  return prepared;
}

static void CopyImageToCanvas(const Magick::Image &image, Canvas *canvas,
                             int x_offset) {
  const int src_w = image.columns();
  const int src_h = image.rows();

  for (int y = 0; y < src_h; ++y) {
    for (int x = 0; x < src_w; ++x) {
      const Magick::Color color = image.pixelColor(x, y);
      if (color.alphaQuantum() >= 256) continue;

      const int r = ScaleQuantumToChar(color.redQuantum());
      const int g = ScaleQuantumToChar(color.greenQuantum());
      const int b = ScaleQuantumToChar(color.blueQuantum());
      if ((r | g | b) == 0) continue;

      const int out_x = x + x_offset;
      if (out_x >= 0 && out_x < canvas->width() && y < canvas->height()) {
        canvas->SetPixel(out_x, y, r, g, b);
      }
    }
  }
}

static void ShowImages(const std::vector<std::string> &files, RGBMatrix *matrix) {
  FrameCanvas *frame = matrix->CreateFrameCanvas();

  while (!interrupt_received) {
    for (const std::string &file : files) {
      if (interrupt_received) return;

      Magick::Image image(file);
      int x_offset = 0;
      image = PrepareImageForMatrix(image, matrix, &x_offset);
      image.scale(Magick::Geometry(matrix->width(), matrix->height()));

      frame->Clear();
      CopyImageToCanvas(image, frame, x_offset);
      frame = matrix->SwapOnVSync(frame);
      usleep(75000);
    }
  }
}

static int usage(const char *progname) {
  fprintf(stderr, "Usage: %s [led-matrix-options] <png-file-or-directory>\n", progname);
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  Magick::InitializeMagick(*argv);

  RGBMatrix::Options matrix_options;
  matrix_options.rows = 64;
  matrix_options.cols = 32;
  matrix_options.brightness = 70;

  rgb_matrix::RuntimeOptions runtime_opt;

  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  if (argc != 2) return usage(argv[0]);

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL) return 1;

  const std::vector<std::string> files = ListPngFiles(argv[1]);
  if (files.empty()) {
    fprintf(stderr, "No PNG files found in '%s'\n", argv[1]);
    delete matrix;
    return 1;
  }

  ShowImages(files, matrix);

  matrix->Clear();
  delete matrix;
  return 0;
}
