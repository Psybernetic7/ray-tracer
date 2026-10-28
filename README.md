# Ray Tracer

A ray tracer written in C++ that renders a scene of diffuse, metal and glass spheres to a PPM image.

![Screenshot](image.png)

## Features

- **Three materials:** Lambertian (diffuse), metal with adjustable fuzz, and dielectric (glass) with refraction and Schlick's reflectance approximation
- **Positionable camera:** configurable vertical field of view, look-from / look-at points and up vector
- **Depth of field:** defocus blur controlled by defocus angle and focus distance
- **Antialiasing:** multiple random samples per pixel
- **Gamma correction:** linear-to-gamma 2 transform on output
- **Randomly generated scene:** a ground sphere, three large feature spheres and a field of small spheres with randomly chosen materials and colors

## Tech Stack

- **Language:** C++
- **Output format:** PPM (P3, plain text), written to standard output
- **Dependencies:** C++ standard library only

## Getting Started

### Prerequisites

A C++ compiler such as `g++` or `clang++`.

### Installation

```bash
git clone https://github.com/Psybernetic7/ray-tracer.git
cd ray-tracer
```

### Build

```bash
g++ -O2 -o main main.cc
```

## Usage

The renderer writes the image to standard output and prints progress to standard error, so redirect the output to a file:

```bash
./main > image.ppm
```

The default scene renders at 1200 px wide (16:9) with 500 samples per pixel and a maximum ray depth of 50, so it can take a while. For a quick preview, lower `image_width` and `samples_per_pixel` in [main.cc](main.cc) and rebuild.

To view the result, open `image.ppm` in an image viewer that supports PPM, or convert it, for example with ImageMagick:

```bash
magick image.ppm image.png
```

### Camera settings

The camera is configured at the bottom of [main.cc](main.cc):

| Setting | Default in `main.cc` | Description |
| --- | --- | --- |
| `aspect_ratio` | `16.0 / 9.0` | Image width over height |
| `image_width` | `1200` | Image width in pixels |
| `samples_per_pixel` | `500` | Random samples per pixel |
| `max_depth` | `50` | Maximum ray bounces |
| `vfov` | `20` | Vertical field of view in degrees |
| `lookfrom` / `lookat` / `vup` | `(13,2,3)` / `(0,0,0)` / `(0,1,0)` | Camera position, target and up direction |
| `defocus_angle` | `0.6` | Variation angle of rays through each pixel |
| `focus_dist` | `10.0` | Distance to the plane of perfect focus |

## Project Structure

```
.
├── main.cc          # Scene setup and camera configuration
├── camera.h         # Camera, ray generation and the ray color loop
├── material.h       # Lambertian, metal and dielectric materials
├── sphere.h         # Sphere primitive
├── hittable.h       # Hit record and hittable interface
├── hittable_list.h  # List of hittable objects
├── vec3.h           # 3D vector, point and color types
├── color.h          # Gamma correction and PPM pixel output
├── ray.h            # Ray type
├── interval.h       # Interval type
└── rtheader.h       # Common includes, constants and utilities
```

## Contributing

Contributions are welcome. Fork the repository, create a branch for your change and open a pull request describing what you changed and why.
