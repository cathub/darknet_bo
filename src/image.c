#include "image.h"
#include "utils.h"
#include "blas.h"
#include "cuda.h"
#include <stdio.h>
#include <math.h>

// Json api library
#include <json-c/json.h>

// Xml api library
// #include <libxml/tree.h>
// #include <libxml/encoding.h>
// #include <libxml/xmlwriter.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define COLOR_NUM 12

int windows = 0;

float colors[6][3] = { {1,0,1}, {0,0,1},{0,1,1},{0,1,0},{1,1,0},{1,0,0} };
const float tiny = 0.000000001;

float get_color(int c, int x, int max)
{
    float ratio = ((float)x/max)*5;
    int i = floor(ratio);
    int j = ceil(ratio);
    ratio -= i;
    float r = (1-ratio) * colors[i][c] + ratio*colors[j][c];
    //printf("%f\n", r);
    return r;
}

image mask_to_rgb(image mask)
{
    int n = mask.c;
    image im = make_image(mask.w, mask.h, 3);
    int i, j;
    for(j = 0; j < n; ++j){
        int offset = j*123457 % n;
        float red = get_color(2,offset,n);
        float green = get_color(1,offset,n);
        float blue = get_color(0,offset,n);
        for(i = 0; i < im.w*im.h; ++i){
            im.data[i + 0*im.w*im.h] += mask.data[j*im.h*im.w + i]*red;
            im.data[i + 1*im.w*im.h] += mask.data[j*im.h*im.w + i]*green;
            im.data[i + 2*im.w*im.h] += mask.data[j*im.h*im.w + i]*blue;
        }
    }
    return im;
}

static float get_pixel(image m, int x, int y, int c)
{
    assert(x < m.w && y < m.h && c < m.c);
    return m.data[c*m.h*m.w + y*m.w + x];
}
static float get_pixel_extend(image m, int x, int y, int c)
{
    if(x < 0 || x >= m.w || y < 0 || y >= m.h) return 0;
    /*
    if(x < 0) x = 0;
    if(x >= m.w) x = m.w-1;
    if(y < 0) y = 0;
    if(y >= m.h) y = m.h-1;
    */
    if(c < 0 || c >= m.c) return 0;
    return get_pixel(m, x, y, c);
}
static void set_pixel(image m, int x, int y, int c, float val)
{
    if (x < 0 || y < 0 || c < 0 || x >= m.w || y >= m.h || c >= m.c) return;
    assert(x < m.w && y < m.h && c < m.c);
    m.data[c*m.h*m.w + y*m.w + x] = val;
}
static void add_pixel(image m, int x, int y, int c, float val)
{
    assert(x < m.w && y < m.h && c < m.c);
    m.data[c*m.h*m.w + y*m.w + x] += val;
}

static float bilinear_interpolate(image im, float x, float y, int c)
{
    int ix = (int) floorf(x);
    int iy = (int) floorf(y);

    float dx = x - ix;
    float dy = y - iy;

    float val = (1-dy) * (1-dx) * get_pixel_extend(im, ix, iy, c) +
        dy     * (1-dx) * get_pixel_extend(im, ix, iy+1, c) +
        (1-dy) *   dx   * get_pixel_extend(im, ix+1, iy, c) +
        dy     *   dx   * get_pixel_extend(im, ix+1, iy+1, c);
    return val;
}


void composite_image(image source, image dest, int dx, int dy)
{
    int x,y,k;
    for(k = 0; k < source.c; ++k){
        for(y = 0; y < source.h; ++y){
            for(x = 0; x < source.w; ++x){
                float val = get_pixel(source, x, y, k);
                float val2 = get_pixel_extend(dest, dx+x, dy+y, k);
                set_pixel(dest, dx+x, dy+y, k, val * val2);
            }
        }
    }
}

image border_image(image a, int border)
{
    image b = make_image(a.w + 2*border, a.h + 2*border, a.c);
    int x,y,k;
    for(k = 0; k < b.c; ++k){
        for(y = 0; y < b.h; ++y){
            for(x = 0; x < b.w; ++x){
                float val = get_pixel_extend(a, x - border, y - border, k);
                if(x - border < 0 || x - border >= a.w || y - border < 0 || y - border >= a.h) val = 1;
                set_pixel(b, x, y, k, val);
            }
        }
    }
    return b;
}

image tile_images(image a, image b, int dx)
{
    if(a.w == 0) return copy_image(b);
    image c = make_image(a.w + b.w + dx, (a.h > b.h) ? a.h : b.h, (a.c > b.c) ? a.c : b.c);
    fill_cpu(c.w*c.h*c.c, 1, c.data, 1);
    embed_image(a, c, 0, 0);
    composite_image(b, c, a.w + dx, 0);
    return c;
}

image get_label(image **characters, char *string, int size)
{
    size = size/10;
    if(size > 7) size = 7;
    image label = make_empty_image(0,0,0);
    while(*string){
        image l = characters[size][(int)*string];
        image n = tile_images(label, l, -size - 1 + (size+1)/2);
        free_image(label);
        label = n;
        ++string;
    }
    image b = border_image(label, label.h*.25);
    free_image(label);
    return b;
}

void draw_label(image a, int r, int c, image label, const float *rgb)
{
    int w = label.w;
    int h = label.h;
    if (r - h >= 0) r = r - h;

    int i, j, k;
    for(j = 0; j < h && j + r < a.h; ++j){
        for(i = 0; i < w && i + c < a.w; ++i){
            for(k = 0; k < label.c; ++k){
                float val = get_pixel(label, i, j, k);
                set_pixel(a, i+c, j+r, k, rgb[k] * val);
            }
        }
    }
}

void draw_box(image a, int x1, int y1, int x2, int y2, float r, float g, float b)
{
    //normalize_image(a);
    int i;
    if(x1 < 0) x1 = 0;
    if(x1 >= a.w) x1 = a.w-1;
    if(x2 < 0) x2 = 0;
    if(x2 >= a.w) x2 = a.w-1;

    if(y1 < 0) y1 = 0;
    if(y1 >= a.h) y1 = a.h-1;
    if(y2 < 0) y2 = 0;
    if(y2 >= a.h) y2 = a.h-1;

    for(i = x1; i <= x2; ++i){
        a.data[i + y1*a.w + 0*a.w*a.h] = r;
        a.data[i + y2*a.w + 0*a.w*a.h] = r;

        a.data[i + y1*a.w + 1*a.w*a.h] = g;
        a.data[i + y2*a.w + 1*a.w*a.h] = g;

        a.data[i + y1*a.w + 2*a.w*a.h] = b;
        a.data[i + y2*a.w + 2*a.w*a.h] = b;
    }
    for(i = y1; i <= y2; ++i){
        a.data[x1 + i*a.w + 0*a.w*a.h] = r;
        a.data[x2 + i*a.w + 0*a.w*a.h] = r;

        a.data[x1 + i*a.w + 1*a.w*a.h] = g;
        a.data[x2 + i*a.w + 1*a.w*a.h] = g;

        a.data[x1 + i*a.w + 2*a.w*a.h] = b;
        a.data[x2 + i*a.w + 2*a.w*a.h] = b;
    }
}

void draw_box_width(image a, int x1, int y1, int x2, int y2, int w, float r, float g, float b)
{
    int i;
    for(i = 0; i < w; ++i){
        draw_box(a, x1+i, y1+i, x2-i, y2-i, r, g, b);
    }
}

void draw_bbox(image a, box bbox, int w, float r, float g, float b)
{
    int left  = (bbox.x-bbox.w/2)*a.w;
    int right = (bbox.x+bbox.w/2)*a.w;
    int top   = (bbox.y-bbox.h/2)*a.h;
    int bot   = (bbox.y+bbox.h/2)*a.h;

    int i;
    for(i = 0; i < w; ++i){
        draw_box(a, left+i, top+i, right-i, bot-i, r, g, b);
    }
}

image **load_alphabet()
{
    int i, j;
    const int nsize = 8;
    image **alphabets = calloc(nsize, sizeof(image));
    for(j = 0; j < nsize; ++j){
        alphabets[j] = calloc(128, sizeof(image));
        for(i = 32; i < 127; ++i){
            char buff[256];
            sprintf(buff, "data/labels/%d_%d.png", i, j);
            alphabets[j][i] = load_image_color(buff, 0, 0);
        }
    }
    return alphabets;
}

float get_average_color(image im, int left, int right, int top, int bot, int c) {
  float result = 0.0;
  for(int j = left; j < right; ++j){
      for(int i = top; i < bot; ++i){
          result += get_pixel(im, j , i, c);
      }
  }
  return 255.0 * result/((right-left)*(bot-top));
}

static char color_name[][64] = {"Black", "White", "Gray",
                                "Red", "Dark Red", "Pink",
                                "Blue", "Dark Blue", "Light Blue",
                                "Yellow", "Orange",
                                "Green"};
static float color_rgb[][3] = {{0,0,0}, {255,255,255}, {64,64,64},
                               {255,0,0}, {139,0,0},{255,153,204},
                               {0,0,255}, {50,40,102},{120,150,160},
                               {255,255,0}, {255,102,0},
                               {0,128,0}};

char* get_color_name(float r, float g, float b) {
  float dist = -1;
  char* color;
  for (int i = 0; i < COLOR_NUM; i++) {
    float d = pow(pow((r - color_rgb[i][0]), 2) +
                  pow((g - color_rgb[i][1]), 2) +
                  pow((b - color_rgb[i][2]), 2), 0.5);
    if (dist < 0 || d < dist) {
      dist = d;
      color = color_name[i];
    }
  }
  return color;
}

// get the color name by:
// 1) assign the pixel color to the nearest color match (KNN)
// 2) count the number of each assigned color numnber
// 3) assign the region color to be the most counted color

char* get_mostcolor_name(image im, int left, int right, int top, int bot) {
  float pixel_color[3];
  float dist = -1;
  int color_count[COLOR_NUM];
  int pixel_color_index;
  int color_max_count = 0;
  int color_max_index = 0;
  char* color;

  for (int i = 0; i < COLOR_NUM; ++i) {
      color_count[i] = 0;
  }

  for(int j = left; j < right; ++j){
      for(int i = top; i < bot; ++i){
          pixel_color[0] = 255.0 * get_pixel(im, j , i, 0); // red
          pixel_color[1] = 255.0 * get_pixel(im, j , i, 1); // green
          pixel_color[2] = 255.0 * get_pixel(im, j , i, 2); // blue

          dist = -1;
          for (int k = 0; k < COLOR_NUM; k++) {
            float d = pow(pow((pixel_color[0] - color_rgb[k][0]), 2) +
                            pow((pixel_color[1] - color_rgb[k][1]), 2) +
                            pow((pixel_color[2] - color_rgb[k][2]), 2), 0.5);
            if (dist < 0 || d < dist) {
                dist = d;
                pixel_color_index = k;
            }
          }
          color_count[pixel_color_index]++;
      }
  }

  for (int i = 0; i < COLOR_NUM; ++i) {
      if (color_count[i] > color_max_count) {
          color_max_count = color_count[i];
          color_max_index = i;
      }
  }
  color = color_name[color_max_index];
  return color;
}

char* get_weighted_mostcolor_name(image im, int left, int right, int top, int bot) {
  int pixel_color[3];
  float dist = -1;
  int color_count[COLOR_NUM];
  int pixel_color_index;
  int color_max_count = 0;
  int color_max_index = 0;
  char* color;
  float r_mean = 0.0;

  for (int i = 0; i < COLOR_NUM; ++i) {
      color_count[i] = 0;
  }

  for(int j = left; j < right; ++j){
      for(int i = top; i < bot; ++i){
          pixel_color[0] = floor(255 * get_pixel(im, j , i, 0)); // red
          pixel_color[1] = floor(255 * get_pixel(im, j , i, 1)); // green
          pixel_color[2] = floor(255 * get_pixel(im, j , i, 2)); // blue

          // ignore background noise color [255, 0 , 255]
          if ( pixel_color[0] == 255 && pixel_color[1] == 0 && pixel_color[2] == 255) {
              continue;
          }

        // printf("r = %d, g = %d, b = %d \n ", pixel_color[0],pixel_color[1], pixel_color[2]);

          dist = -1;
          for (int k = 0; k < COLOR_NUM; k++) {
            r_mean = (pixel_color[0] + color_rgb[k][0]) / 2;
            float d = pow((2 + r_mean/256) * pow((pixel_color[0] - color_rgb[k][0]), 2) +
                      4 * pow((pixel_color[1] - color_rgb[k][1]), 2) + (2 + (255 - r_mean)/256) * pow((pixel_color[2] - color_rgb[k][2]), 2), 0.5);

            if (dist < 0 || d < dist) {
                dist = d;
                pixel_color_index = k;
            }
          }
          color_count[pixel_color_index]++;
      }
  }
  for (int i = 0; i < COLOR_NUM; ++i) {
      if (color_count[i] > color_max_count) {
          color_max_count = color_count[i];
          color_max_index = i;
      }
     //  printf("color_count %d = %d\n", i, color_count[i]);
  }
  color = color_name[color_max_index];
  return color;
}

static float person_cen_prev[][2] = {{0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0},
                                     {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}};
float point_dist(float x1, float y1, float x2, float y2) {
  return pow(pow(x1-x2, 2) + pow(y1-y2, 2), 0.5);
}

void swap(int *xp, int *yp)
{
    int temp = *xp;
    *xp = *yp;
    *yp = temp;
}

void draw_detections(image im, detection *dets, int num, float thresh, char **names, image **alphabet, int classes, double time_index)
{
    int person_index = 0;
    // dist[i][j] represents the dist of person i to previous center j.
    float dist[][12] = {{0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0},
                        {0,0,0,0,0,0,0,0,0,0,0,0}};

   // current frame
   // obj index of person_index i.
   int obj_index[12] = {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
   float person_cen_cur[][2] = {{0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0}, {0,0},
                                {0,0}, {0,0}, {0,0}, {0,0}, {0,0}};

    for(int i = 0; i < num; ++i){
        char labelstr[4096] = {0};
        int class = -1;
        for(int j = 0; j < classes; ++j){
            if (dets[i].prob[j] > thresh){
                if (class < 0) {
                    strcat(labelstr, names[j]);
                    class = j;
                } else {
                    strcat(labelstr, ", ");
                    strcat(labelstr, names[j]);
                }
                printf("%s: %.0f%%\n", names[j], dets[i].prob[j]*100);
            }
        }
        if(class >= 0){
            box b = dets[i].bbox;

            int left  = (b.x-b.w/2.)*im.w;
            int right = (b.x+b.w/2.)*im.w;
            int top   = (b.y-b.h/2.)*im.h;
            int bot   = (b.y+b.h/2.)*im.h;

            if(left < 0) left = 0;
            if(right > im.w-1) right = im.w-1;
            if(top < 0) top = 0;
            if(bot > im.h-1) bot = im.h-1;

            char *output = NULL;
            output = strstr (labelstr, "person");

            // if this is person.
            if (output != NULL) {
              if (person_index < 12) {
                float x = (left + right)/2.0;
                float y = (top + bot)/2.0;
                person_cen_cur[person_index][0] = x;
                person_cen_cur[person_index][1] = y;
                obj_index[person_index] = i;
                for(int m = 0; m < 12; m++) {
                   // previous center m is valid
                   if (person_cen_prev[m][0] > tiny ||
                       person_cen_prev[m][1] > tiny) {
                     dist[person_index][m] = point_dist(x, y,
                                                        person_cen_prev[m][0],
                                                        person_cen_prev[m][1]);
                   }
                }
                person_index++;
              }
              printf("Detecting person %d\n", person_index);
              continue;
            }

            // if this is not person.
            int width = im.h * .006;
            int offset = class*123457 % classes;
            float red = get_color(2,offset,classes);
            float green = get_color(1,offset,classes);
            float blue = get_color(0,offset,classes);
            float rgb[3];

            rgb[0] = red;
            rgb[1] = green;
            rgb[2] = blue;


            draw_box_width(im, left, top, right, bot, width, red, green, blue);
            if (alphabet) {
                image label = get_label(alphabet, labelstr, (im.h*.03));
                draw_label(im, top + width, left, label, rgb);
                free_image(label);
            }
            if (dets[i].mask){
                image mask = float_to_image(14, 14, 1, dets[i].mask);
                image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
                image tmask = threshold_image(resized_mask, .5);
                embed_image(tmask, im, left, top);
                free_image(mask);
                free_image(resized_mask);
                free_image(tmask);
            }
        }
    }
   // sort previous centers based on distance of previous center to the image
   // center.
   int sort_array[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
   float center_x = im.w/2.0;
   float center_y = im.h/2.0;
   for (int i = 0; i < 11; i++) {
       // Last i elements are already in place
       for (int j = 0; j < 11-i; j++) {
           int a = sort_array[j];
           int b = sort_array[j+1];
           if (person_cen_prev[a][0] < tiny &&
               person_cen_prev[a][1] < tiny) {

             swap(&sort_array[j], &sort_array[j+1]);
           }
           if (person_cen_prev[b][0] < tiny &&
               person_cen_prev[b][1] < tiny) {
             continue;
           }

           if (point_dist(center_x, center_y, person_cen_prev[a][0],
                          person_cen_prev[a][1]) <
               point_dist(center_x, center_y, person_cen_prev[b][0],
                          person_cen_prev[b][1])) {
             swap(&sort_array[j], &sort_array[j+1]);
           }
       }
   }

   // process person
    for(int i = 0; i < 12; i ++) {
      int prev_index = sort_array[i];
      if (person_cen_prev[prev_index][0] < tiny &&
          person_cen_prev[prev_index][1] < tiny) continue;

      int assigned_person = -1;
      float min_dist = -1.0;
      for(int j = 0; j < person_index; j++) {
        // person j has already been assigned.
        if (obj_index[j] == -1) continue;
        if (min_dist < 0 || (dist[j][i] < min_dist)){
          min_dist = dist[j][i];
          assigned_person = j;
        }
      }
      if (assigned_person == -1) {
        person_cen_prev[i][0] = 0;
        person_cen_prev[i][1] = 0;
        continue;
      }

      // update the new center
      person_cen_prev[i][0] = person_cen_cur[assigned_person][0];
      person_cen_prev[i][1] = person_cen_cur[assigned_person][1];

      char person_label[64];
      snprintf(person_label, sizeof(person_label), "person_%d", i);
      int obj_ind = obj_index[assigned_person];
      obj_index[assigned_person] = -1;
      box bp = dets[obj_ind].bbox;

      int left  = (bp.x-bp.w/2.)*im.w;
      int right = (bp.x+bp.w/2.)*im.w;
      int top   = (bp.y-bp.h/2.)*im.h;
      int bot   = (bp.y+bp.h/2.)*im.h;

      int width = im.h * .006;
      int offset = i * 123457 % 12;
      float red = get_color(2,offset,12);
      float green = get_color(1,offset,12);
      float blue = get_color(0,offset,12);
      float rgb[3];

      rgb[0] = red;
      rgb[1] = green;
      rgb[2] = blue;

      draw_box_width(im, left, top, right, bot, width, red, green, blue);
      if (alphabet) {
          image label = get_label(alphabet, person_label, (im.h*.03));
          draw_label(im, top + width, left, label, rgb);
          free_image(label);
      }
      if (dets[obj_ind].mask){
          image mask = float_to_image(14, 14, 1, dets[obj_ind].mask);
          image resized_mask = resize_image(mask, bp.w*im.w, bp.h*im.h);
          image tmask = threshold_image(resized_mask, .5);
          embed_image(tmask, im, left, top);
          free_image(mask);
          free_image(resized_mask);
          free_image(tmask);
      }
    }

    // process left over person.
    for(int i = 0; i < person_index; i++) {
      if (obj_index[i] == -1) continue;
      for (int j = 0; j < 12; j++) {
        // if j is not assigned. take it.
        if (person_cen_prev[j][0] < tiny && person_cen_prev[j][1] < tiny) {
          // update the new center
          printf("assign slot %d\n", j);
          person_cen_prev[j][0] = person_cen_cur[i][0];
          person_cen_prev[j][1] = person_cen_cur[i][1];

          printf("after assign slot\n");
          char person_label[64];
          snprintf(person_label, sizeof(person_label), "person_%d", j);
          int obj_ind = obj_index[i];
          box bp = dets[obj_ind].bbox;

          printf("after bp\n");
          int left  = (bp.x-bp.w/2.)*im.w;
          int right = (bp.x+bp.w/2.)*im.w;
          int top   = (bp.y-bp.h/2.)*im.h;
          int bot   = (bp.y+bp.h/2.)*im.h;

          printf("before assign width\n");
          int width = im.h * .006;
          int offset = j * 123457 % 12;
          float red = get_color(2,offset,12);
          float green = get_color(1,offset,12);
          float blue = get_color(0,offset,12);
          float rgb[3];

          rgb[0] = red;
          rgb[1] = green;
          rgb[2] = blue;

          printf("before draw_box_width\n");
          draw_box_width(im, left, top, right, bot, width, red, green, blue);
          if (alphabet) {
              image label = get_label(alphabet, person_label, (im.h*.03));
              draw_label(im, top + width, left, label, rgb);
              free_image(label);
          }
          if (dets[obj_ind].mask){
              image mask = float_to_image(14, 14, 1, dets[obj_ind].mask);
              image resized_mask = resize_image(mask, bp.w*im.w, bp.h*im.h);
              image tmask = threshold_image(resized_mask, .5);
              embed_image(tmask, im, left, top);
              free_image(mask);
              free_image(resized_mask);
              free_image(tmask);
          }
          obj_index[i] = -1;
          break;
        }
      }
    }
}
//
// void draw_detections(image im, detection *dets, int num, float thresh, char **names, image **alphabet, int classes, double time_index)
// {
//     int i,j;
//     struct json_object *json_obj = json_object_new_object();
//     char file_name[64];
//     int time_stamp = floor(time_index);
//     snprintf(file_name, sizeof(file_name), "output-%d.json", time_stamp);
//     FILE *file = fopen(file_name, "w+");
//     if(file == 0) file_error(file_name);
//     int obj_index = 1;
//     int person_index = 0;
//
//     for(i = 0; i < num; ++i){
//         char labelstr[4096] = {0};
//         int class = -1;
//         for(j = 0; j < classes; ++j){
//             if (dets[i].prob[j] > thresh){
//                 if (class < 0) {
//                     strcat(labelstr, names[j]);
//                     class = j;
//                 } else {
//                     strcat(labelstr, ", ");
//                     strcat(labelstr, names[j]);
//                 }
//                 printf("%s: %.0f%%\n", names[j], dets[i].prob[j]*100);
//             }
//         }
//         if(class >= 0){
//             int width = im.h * .006;
//             int offset = class*123457 % classes;
//             float red = get_color(2,offset,classes);
//             float green = get_color(1,offset,classes);
//             float blue = get_color(0,offset,classes);
//             float rgb[3];
//
//             rgb[0] = red;
//             rgb[1] = green;
//             rgb[2] = blue;
//             box b = dets[i].bbox;
//
//             int left  = (b.x-b.w/2.)*im.w;
//             int right = (b.x+b.w/2.)*im.w;
//             int top   = (b.y-b.h/2.)*im.h;
//             int bot   = (b.y+b.h/2.)*im.h;
//
//             if(left < 0) left = 0;
//             if(right > im.w-1) right = im.w-1;
//             if(top < 0) top = 0;
//             if(bot > im.h-1) bot = im.h-1;
//
//             char *output = NULL;
//             output = strstr (labelstr, "person");
//
//             if (output != NULL) {
//               person_index++;   // one more person detected
//
//               struct json_object *json_person = json_object_new_object();
//               int neck = top + (bot - top)/8;
//               int waist = top + (bot - top)/2;
//               int ankle = top + (bot - top)/10*9;
//
//               char head[64];
//               int head_left = left + (right - left) * 0.3;
//               int head_right = right - (right-left) * 0.3;
//               int head_top = top + (neck - top) * 0.2;
//               int head_bot = neck - (neck - top) * 0.2;
//
//             // printf("Detecting person %d head\n", person_index);
//               draw_box_width(im, head_left, head_top, head_right, head_bot, width, red, green, blue);
//               snprintf(head, sizeof(head), "%s",
//                        get_weighted_mostcolor_name(im, head_left, head_right, head_top, head_bot));
//
//               struct json_object *json_head = json_object_new_string(head);
//               json_object_object_add(json_person, "head_color", json_head);
//
//               if (alphabet) {
//                   image label = get_label(alphabet, head, (im.h*.01));
//                   draw_label(im, head_top + width, head_left, label, rgb);
//                   free_image(label);
//               }
//               if (dets[i].mask){
//                   image mask = float_to_image(14, 14, 1, dets[i].mask);
//                   image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
//                   image tmask = threshold_image(resized_mask, .5);
//                   embed_image(tmask, im, head_left, head_top);
//                   free_image(mask);
//                   free_image(resized_mask);
//                   free_image(tmask);
//               }
//
//               char upper_body[64];
//               int upper_body_left = left + (right - left) * 0.3;
//               int upper_body_right = right - (right-left) * 0.3;
//               int upper_body_top = neck + (waist - neck) * 0.3;
//               int upper_body_bot = waist - (waist - neck) * 0.2;
//
//             //   printf("Detecting person %d upper_body\n", person_index);
//               draw_box_width(im, upper_body_left, upper_body_top, upper_body_right, upper_body_bot, width, red, green, blue);
//               snprintf(upper_body, sizeof(upper_body), "%s",
//                        get_weighted_mostcolor_name(im, upper_body_left, upper_body_right, upper_body_top, upper_body_bot));
//
//               struct json_object *json_upper_body = json_object_new_string(upper_body);
//               json_object_object_add(json_person, "upper_body_color", json_upper_body);
//
//               if (alphabet) {
//                   image label = get_label(alphabet, upper_body, (im.h*.01));
//                   draw_label(im, upper_body_top + width, upper_body_left, label, rgb);
//                   free_image(label);
//               }
//               if (dets[i].mask){
//                   image mask = float_to_image(14, 14, 1, dets[i].mask);
//                   image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
//                   image tmask = threshold_image(resized_mask, .5);
//                   embed_image(tmask, im, upper_body_left, upper_body_top);
//                   free_image(mask);
//                   free_image(resized_mask);
//                   free_image(tmask);
//               }
//
//               char bottom_body[64];
//               int bottom_body_left = left + (right - left) * 0.3;
//               int bottom_body_right = right - (right-left) * 0.3;
//               int bottom_body_top = waist + (ankle - waist) * 0.2;
//               int bottom_body_bot = ankle - (ankle - waist) * 0.4;
//
//              ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//             //   // single person debugging
//
//             //   if (person_index < 8) {
//             //       printf("Detecting person %d bottom\n", person_index);
//             //       draw_box_width(im, bottom_body_left, bottom_body_top, bottom_body_right, bottom_body_bot, width, red, green, blue);
//
//             //       snprintf(bottom_body, sizeof(bottom_body), "%s",
//             //            get_weighted_mostcolor_name(im, bottom_body_left, bottom_body_right, bottom_body_top, bottom_body_bot));
//
//             //       struct json_object *json_bottom_body = json_object_new_string(bottom_body);
//             //       json_object_object_add(json_person, "bottom_body_color", json_bottom_body);
//
//             //       if (alphabet) {
//             //           image label = get_label(alphabet, bottom_body, (im.h*.01));
//             //           draw_label(im, bottom_body_top + width, bottom_body_left, label, rgb);
//             //           free_image(label);
//             //         }
//
//             //       if (dets[i].mask){
//             //           image mask = float_to_image(14, 14, 1, dets[i].mask);
//             //           image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
//             //           image tmask = threshold_image(resized_mask, .5);
//             //           embed_image(tmask, im, bottom_body_left, bottom_body_top);
//             //           free_image(mask);
//             //           free_image(resized_mask);
//             //           free_image(tmask);
//             //         }
//             //     }
//
//             ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//             // printf("Detecting person %d bottom\n", person_index);
//               draw_box_width(im, bottom_body_left, bottom_body_top, bottom_body_right, bottom_body_bot, width, red, green, blue);
//               snprintf(bottom_body, sizeof(bottom_body), "%s",
//                       get_weighted_mostcolor_name(im, bottom_body_left, bottom_body_right, bottom_body_top, bottom_body_bot));
//
//               struct json_object *json_bottom_body = json_object_new_string(bottom_body);
//               json_object_object_add(json_person, "bottom_body_color", json_bottom_body);
//
//               if (alphabet) {
//                   image label = get_label(alphabet, bottom_body, (im.h*.01));
//                   draw_label(im, bottom_body_top + width, bottom_body_right, label, rgb);
//                   free_image(label);
//               }
//               if (dets[i].mask){
//                   image mask = float_to_image(14, 14, 1, dets[i].mask);
//                   image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
//                   image tmask = threshold_image(resized_mask, .5);
//                   embed_image(tmask, im, bottom_body_left, bottom_body_top);
//                   free_image(mask);
//                   free_image(resized_mask);
//                   free_image(tmask);
//               }
//
//               char foot[64];
//               int foot_left = left + (right - left) * 0.4;
//               int foot_right = right - (right - left) * 0.4;
//               int foot_top = ankle + (bot - ankle) * 0.3;
//               int foot_bot = bot - (bot - ankle) * 0;
//
//             //   printf("Detecting person %d foot\n", person_index);
//               draw_box_width(im, foot_left, foot_top, foot_right, foot_bot, width, red, green, blue);
//               snprintf(foot, sizeof(foot), "%s",
//                        get_weighted_mostcolor_name(im, foot_left, foot_right, foot_top, foot_bot));
//
//               struct json_object *json_foot = json_object_new_string(foot);
//               json_object_object_add(json_person, "foot_color", json_foot);
//
//               if (alphabet) {
//                   image label = get_label(alphabet, foot, (im.h*.01));
//                   draw_label(im, foot_top + width, foot_left, label, rgb);
//                   free_image(label);
//               }
//               if (dets[i].mask){
//                   image mask = float_to_image(14, 14, 1, dets[i].mask);
//                   image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
//                   image tmask = threshold_image(resized_mask, .5);
//                   embed_image(tmask, im, foot_left, foot_bot);
//                   free_image(mask);
//                   free_image(resized_mask);
//                   free_image(tmask);
//               }
//
//               char person_object[64];
//               sprintf(person_object, "person %d", person_index);
//               json_object_object_add(json_obj, person_object, json_person);
//
//               continue;
//             }
//
//             char object[64];
//             sprintf(object, "object %d", obj_index++);
//             json_object_object_add(json_obj, object, json_object_new_string(labelstr));
//
//             draw_box_width(im, left, top, right, bot, width, red, green, blue);
//             if (alphabet) {
//                 image label = get_label(alphabet, labelstr, (im.h*.03));
//                 draw_label(im, top + width, left, label, rgb);
//                 free_image(label);
//             }
//             if (dets[i].mask){
//                 image mask = float_to_image(14, 14, 1, dets[i].mask);
//                 image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
//                 image tmask = threshold_image(resized_mask, .5);
//                 embed_image(tmask, im, left, top);
//                 free_image(mask);
//                 free_image(resized_mask);
//                 free_image(tmask);
//             }
//         }
//     }
//     fprintf(file, "%s", json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_PRETTY));
//     printf(json_object_to_json_string_ext(json_obj, JSON_C_TO_STRING_SPACED | JSON_C_TO_STRING_PRETTY));
//     fclose(file);
// }

void transpose_image(image im)
{
    assert(im.w == im.h);
    int n, m;
    int c;
    for(c = 0; c < im.c; ++c){
        for(n = 0; n < im.w-1; ++n){
            for(m = n + 1; m < im.w; ++m){
                float swap = im.data[m + im.w*(n + im.h*c)];
                im.data[m + im.w*(n + im.h*c)] = im.data[n + im.w*(m + im.h*c)];
                im.data[n + im.w*(m + im.h*c)] = swap;
            }
        }
    }
}

void rotate_image_cw(image im, int times)
{
    assert(im.w == im.h);
    times = (times + 400) % 4;
    int i, x, y, c;
    int n = im.w;
    for(i = 0; i < times; ++i){
        for(c = 0; c < im.c; ++c){
            for(x = 0; x < n/2; ++x){
                for(y = 0; y < (n-1)/2 + 1; ++y){
                    float temp = im.data[y + im.w*(x + im.h*c)];
                    im.data[y + im.w*(x + im.h*c)] = im.data[n-1-x + im.w*(y + im.h*c)];
                    im.data[n-1-x + im.w*(y + im.h*c)] = im.data[n-1-y + im.w*(n-1-x + im.h*c)];
                    im.data[n-1-y + im.w*(n-1-x + im.h*c)] = im.data[x + im.w*(n-1-y + im.h*c)];
                    im.data[x + im.w*(n-1-y + im.h*c)] = temp;
                }
            }
        }
    }
}

void flip_image(image a)
{
    int i,j,k;
    for(k = 0; k < a.c; ++k){
        for(i = 0; i < a.h; ++i){
            for(j = 0; j < a.w/2; ++j){
                int index = j + a.w*(i + a.h*(k));
                int flip = (a.w - j - 1) + a.w*(i + a.h*(k));
                float swap = a.data[flip];
                a.data[flip] = a.data[index];
                a.data[index] = swap;
            }
        }
    }
}

image image_distance(image a, image b)
{
    int i,j;
    image dist = make_image(a.w, a.h, 1);
    for(i = 0; i < a.c; ++i){
        for(j = 0; j < a.h*a.w; ++j){
            dist.data[j] += pow(a.data[i*a.h*a.w+j]-b.data[i*a.h*a.w+j],2);
        }
    }
    for(j = 0; j < a.h*a.w; ++j){
        dist.data[j] = sqrt(dist.data[j]);
    }
    return dist;
}

void ghost_image(image source, image dest, int dx, int dy)
{
    int x,y,k;
    float max_dist = sqrt((-source.w/2. + .5)*(-source.w/2. + .5));
    for(k = 0; k < source.c; ++k){
        for(y = 0; y < source.h; ++y){
            for(x = 0; x < source.w; ++x){
                float dist = sqrt((x - source.w/2. + .5)*(x - source.w/2. + .5) + (y - source.h/2. + .5)*(y - source.h/2. + .5));
                float alpha = (1 - dist/max_dist);
                if(alpha < 0) alpha = 0;
                float v1 = get_pixel(source, x,y,k);
                float v2 = get_pixel(dest, dx+x,dy+y,k);
                float val = alpha*v1 + (1-alpha)*v2;
                set_pixel(dest, dx+x, dy+y, k, val);
            }
        }
    }
}

void blocky_image(image im, int s)
{
    int i,j,k;
    for(k = 0; k < im.c; ++k){
        for(j = 0; j < im.h; ++j){
            for(i = 0; i < im.w; ++i){
                im.data[i + im.w*(j + im.h*k)] = im.data[i/s*s + im.w*(j/s*s + im.h*k)];
            }
        }
    }
}

void censor_image(image im, int dx, int dy, int w, int h)
{
    int i,j,k;
    int s = 32;
    if(dx < 0) dx = 0;
    if(dy < 0) dy = 0;

    for(k = 0; k < im.c; ++k){
        for(j = dy; j < dy + h && j < im.h; ++j){
            for(i = dx; i < dx + w && i < im.w; ++i){
                im.data[i + im.w*(j + im.h*k)] = im.data[i/s*s + im.w*(j/s*s + im.h*k)];
                //im.data[i + j*im.w + k*im.w*im.h] = 0;
            }
        }
    }
}

void embed_image(image source, image dest, int dx, int dy)
{
    int x,y,k;
    for(k = 0; k < source.c; ++k){
        for(y = 0; y < source.h; ++y){
            for(x = 0; x < source.w; ++x){
                float val = get_pixel(source, x,y,k);
                set_pixel(dest, dx+x, dy+y, k, val);
            }
        }
    }
}

image collapse_image_layers(image source, int border)
{
    int h = source.h;
    h = (h+border)*source.c - border;
    image dest = make_image(source.w, h, 1);
    int i;
    for(i = 0; i < source.c; ++i){
        image layer = get_image_layer(source, i);
        int h_offset = i*(source.h+border);
        embed_image(layer, dest, 0, h_offset);
        free_image(layer);
    }
    return dest;
}

void constrain_image(image im)
{
    int i;
    for(i = 0; i < im.w*im.h*im.c; ++i){
        if(im.data[i] < 0) im.data[i] = 0;
        if(im.data[i] > 1) im.data[i] = 1;
    }
}

void normalize_image(image p)
{
    int i;
    float min = 9999999;
    float max = -999999;

    for(i = 0; i < p.h*p.w*p.c; ++i){
        float v = p.data[i];
        if(v < min) min = v;
        if(v > max) max = v;
    }
    if(max - min < .000000001){
        min = 0;
        max = 1;
    }
    for(i = 0; i < p.c*p.w*p.h; ++i){
        p.data[i] = (p.data[i] - min)/(max-min);
    }
}

void normalize_image2(image p)
{
    float *min = calloc(p.c, sizeof(float));
    float *max = calloc(p.c, sizeof(float));
    int i,j;
    for(i = 0; i < p.c; ++i) min[i] = max[i] = p.data[i*p.h*p.w];

    for(j = 0; j < p.c; ++j){
        for(i = 0; i < p.h*p.w; ++i){
            float v = p.data[i+j*p.h*p.w];
            if(v < min[j]) min[j] = v;
            if(v > max[j]) max[j] = v;
        }
    }
    for(i = 0; i < p.c; ++i){
        if(max[i] - min[i] < .000000001){
            min[i] = 0;
            max[i] = 1;
        }
    }
    for(j = 0; j < p.c; ++j){
        for(i = 0; i < p.w*p.h; ++i){
            p.data[i+j*p.h*p.w] = (p.data[i+j*p.h*p.w] - min[j])/(max[j]-min[j]);
        }
    }
    free(min);
    free(max);
}

void copy_image_into(image src, image dest)
{
    memcpy(dest.data, src.data, src.h*src.w*src.c*sizeof(float));
}

image copy_image(image p)
{
    image copy = p;
    copy.data = calloc(p.h*p.w*p.c, sizeof(float));
    memcpy(copy.data, p.data, p.h*p.w*p.c*sizeof(float));
    return copy;
}

void rgbgr_image(image im)
{
    int i;
    for(i = 0; i < im.w*im.h; ++i){
        float swap = im.data[i];
        im.data[i] = im.data[i+im.w*im.h*2];
        im.data[i+im.w*im.h*2] = swap;
    }
}

int show_image(image p, const char *name, int ms)
{
#ifdef OPENCV
    int c = show_image_cv(p, name, ms);
    return c;
#else
    fprintf(stderr, "Not compiled with OpenCV, saving to %s.png instead\n", name);
    save_image(p, name);
    return -1;
#endif
}

void save_image_options(image im, const char *name, IMTYPE f, int quality)
{
    char buff[256];
    //sprintf(buff, "%s (%d)", name, windows);
    if(f == PNG)       sprintf(buff, "%s.png", name);
    else if (f == BMP) sprintf(buff, "%s.bmp", name);
    else if (f == TGA) sprintf(buff, "%s.tga", name);
    else if (f == JPG) sprintf(buff, "%s.jpg", name);
    else               sprintf(buff, "%s.png", name);
    unsigned char *data = calloc(im.w*im.h*im.c, sizeof(char));
    int i,k;
    for(k = 0; k < im.c; ++k){
        for(i = 0; i < im.w*im.h; ++i){
            data[i*im.c+k] = (unsigned char) (255*im.data[i + k*im.w*im.h]);
        }
    }
    int success = 0;
    if(f == PNG)       success = stbi_write_png(buff, im.w, im.h, im.c, data, im.w*im.c);
    else if (f == BMP) success = stbi_write_bmp(buff, im.w, im.h, im.c, data);
    else if (f == TGA) success = stbi_write_tga(buff, im.w, im.h, im.c, data);
    else if (f == JPG) success = stbi_write_jpg(buff, im.w, im.h, im.c, data, quality);
    free(data);
    if(!success) fprintf(stderr, "Failed to write image %s\n", buff);
}

void save_image(image im, const char *name)
{
    save_image_options(im, name, JPG, 80);
}

void show_image_layers(image p, char *name)
{
    int i;
    char buff[256];
    for(i = 0; i < p.c; ++i){
        sprintf(buff, "%s - Layer %d", name, i);
        image layer = get_image_layer(p, i);
        show_image(layer, buff, 1);
        free_image(layer);
    }
}

void show_image_collapsed(image p, char *name)
{
    image c = collapse_image_layers(p, 1);
    show_image(c, name, 1);
    free_image(c);
}

image make_empty_image(int w, int h, int c)
{
    image out;
    out.data = 0;
    out.h = h;
    out.w = w;
    out.c = c;
    return out;
}

image make_image(int w, int h, int c)
{
    image out = make_empty_image(w,h,c);
    out.data = calloc(h*w*c, sizeof(float));
    return out;
}

image make_random_image(int w, int h, int c)
{
    image out = make_empty_image(w,h,c);
    out.data = calloc(h*w*c, sizeof(float));
    int i;
    for(i = 0; i < w*h*c; ++i){
        out.data[i] = (rand_normal() * .25) + .5;
    }
    return out;
}

image float_to_image(int w, int h, int c, float *data)
{
    image out = make_empty_image(w,h,c);
    out.data = data;
    return out;
}

void place_image(image im, int w, int h, int dx, int dy, image canvas)
{
    int x, y, c;
    for(c = 0; c < im.c; ++c){
        for(y = 0; y < h; ++y){
            for(x = 0; x < w; ++x){
                float rx = ((float)x / w) * im.w;
                float ry = ((float)y / h) * im.h;
                float val = bilinear_interpolate(im, rx, ry, c);
                set_pixel(canvas, x + dx, y + dy, c, val);
            }
        }
    }
}

image center_crop_image(image im, int w, int h)
{
    int m = (im.w < im.h) ? im.w : im.h;
    image c = crop_image(im, (im.w - m) / 2, (im.h - m)/2, m, m);
    image r = resize_image(c, w, h);
    free_image(c);
    return r;
}

image rotate_crop_image(image im, float rad, float s, int w, int h, float dx, float dy, float aspect)
{
    int x, y, c;
    float cx = im.w/2.;
    float cy = im.h/2.;
    image rot = make_image(w, h, im.c);
    for(c = 0; c < im.c; ++c){
        for(y = 0; y < h; ++y){
            for(x = 0; x < w; ++x){
                float rx = cos(rad)*((x - w/2.)/s*aspect + dx/s*aspect) - sin(rad)*((y - h/2.)/s + dy/s) + cx;
                float ry = sin(rad)*((x - w/2.)/s*aspect + dx/s*aspect) + cos(rad)*((y - h/2.)/s + dy/s) + cy;
                float val = bilinear_interpolate(im, rx, ry, c);
                set_pixel(rot, x, y, c, val);
            }
        }
    }
    return rot;
}

image rotate_image(image im, float rad)
{
    int x, y, c;
    float cx = im.w/2.;
    float cy = im.h/2.;
    image rot = make_image(im.w, im.h, im.c);
    for(c = 0; c < im.c; ++c){
        for(y = 0; y < im.h; ++y){
            for(x = 0; x < im.w; ++x){
                float rx = cos(rad)*(x-cx) - sin(rad)*(y-cy) + cx;
                float ry = sin(rad)*(x-cx) + cos(rad)*(y-cy) + cy;
                float val = bilinear_interpolate(im, rx, ry, c);
                set_pixel(rot, x, y, c, val);
            }
        }
    }
    return rot;
}

void fill_image(image m, float s)
{
    int i;
    for(i = 0; i < m.h*m.w*m.c; ++i) m.data[i] = s;
}

void translate_image(image m, float s)
{
    int i;
    for(i = 0; i < m.h*m.w*m.c; ++i) m.data[i] += s;
}

void scale_image(image m, float s)
{
    int i;
    for(i = 0; i < m.h*m.w*m.c; ++i) m.data[i] *= s;
}

image crop_image(image im, int dx, int dy, int w, int h)
{
    image cropped = make_image(w, h, im.c);
    int i, j, k;
    for(k = 0; k < im.c; ++k){
        for(j = 0; j < h; ++j){
            for(i = 0; i < w; ++i){
                int r = j + dy;
                int c = i + dx;
                float val = 0;
                r = constrain_int(r, 0, im.h-1);
                c = constrain_int(c, 0, im.w-1);
                val = get_pixel(im, c, r, k);
                set_pixel(cropped, i, j, k, val);
            }
        }
    }
    return cropped;
}

int best_3d_shift_r(image a, image b, int min, int max)
{
    if(min == max) return min;
    int mid = floor((min + max) / 2.);
    image c1 = crop_image(b, 0, mid, b.w, b.h);
    image c2 = crop_image(b, 0, mid+1, b.w, b.h);
    float d1 = dist_array(c1.data, a.data, a.w*a.h*a.c, 10);
    float d2 = dist_array(c2.data, a.data, a.w*a.h*a.c, 10);
    free_image(c1);
    free_image(c2);
    if(d1 < d2) return best_3d_shift_r(a, b, min, mid);
    else return best_3d_shift_r(a, b, mid+1, max);
}

int best_3d_shift(image a, image b, int min, int max)
{
    int i;
    int best = 0;
    float best_distance = FLT_MAX;
    for(i = min; i <= max; i += 2){
        image c = crop_image(b, 0, i, b.w, b.h);
        float d = dist_array(c.data, a.data, a.w*a.h*a.c, 100);
        if(d < best_distance){
            best_distance = d;
            best = i;
        }
        printf("%d %f\n", i, d);
        free_image(c);
    }
    return best;
}

void composite_3d(char *f1, char *f2, char *out, int delta)
{
    if(!out) out = "out";
    image a = load_image(f1, 0,0,0);
    image b = load_image(f2, 0,0,0);
    int shift = best_3d_shift_r(a, b, -a.h/100, a.h/100);

    image c1 = crop_image(b, 10, shift, b.w, b.h);
    float d1 = dist_array(c1.data, a.data, a.w*a.h*a.c, 100);
    image c2 = crop_image(b, -10, shift, b.w, b.h);
    float d2 = dist_array(c2.data, a.data, a.w*a.h*a.c, 100);

    if(d2 < d1 && 0){
        image swap = a;
        a = b;
        b = swap;
        shift = -shift;
        printf("swapped, %d\n", shift);
    }
    else{
        printf("%d\n", shift);
    }

    image c = crop_image(b, delta, shift, a.w, a.h);
    int i;
    for(i = 0; i < c.w*c.h; ++i){
        c.data[i] = a.data[i];
    }
    save_image(c, out);
}

void letterbox_image_into(image im, int w, int h, image boxed)
{
    int new_w = im.w;
    int new_h = im.h;
    if (((float)w/im.w) < ((float)h/im.h)) {
        new_w = w;
        new_h = (im.h * w)/im.w;
    } else {
        new_h = h;
        new_w = (im.w * h)/im.h;
    }
    image resized = resize_image(im, new_w, new_h);
    embed_image(resized, boxed, (w-new_w)/2, (h-new_h)/2);
    free_image(resized);
}

image letterbox_image(image im, int w, int h)
{
    int new_w = im.w;
    int new_h = im.h;
    if (((float)w/im.w) < ((float)h/im.h)) {
        new_w = w;
        new_h = (im.h * w)/im.w;
    } else {
        new_h = h;
        new_w = (im.w * h)/im.h;
    }
    image resized = resize_image(im, new_w, new_h);
    image boxed = make_image(w, h, im.c);
    fill_image(boxed, .5);
    //int i;
    //for(i = 0; i < boxed.w*boxed.h*boxed.c; ++i) boxed.data[i] = 0;
    embed_image(resized, boxed, (w-new_w)/2, (h-new_h)/2);
    free_image(resized);
    return boxed;
}

image resize_max(image im, int max)
{
    int w = im.w;
    int h = im.h;
    if(w > h){
        h = (h * max) / w;
        w = max;
    } else {
        w = (w * max) / h;
        h = max;
    }
    if(w == im.w && h == im.h) return im;
    image resized = resize_image(im, w, h);
    return resized;
}

image resize_min(image im, int min)
{
    int w = im.w;
    int h = im.h;
    if(w < h){
        h = (h * min) / w;
        w = min;
    } else {
        w = (w * min) / h;
        h = min;
    }
    if(w == im.w && h == im.h) return im;
    image resized = resize_image(im, w, h);
    return resized;
}

image random_crop_image(image im, int w, int h)
{
    int dx = rand_int(0, im.w - w);
    int dy = rand_int(0, im.h - h);
    image crop = crop_image(im, dx, dy, w, h);
    return crop;
}

augment_args random_augment_args(image im, float angle, float aspect, int low, int high, int w, int h)
{
    augment_args a = {0};
    aspect = rand_scale(aspect);
    int r = rand_int(low, high);
    int min = (im.h < im.w*aspect) ? im.h : im.w*aspect;
    float scale = (float)r / min;

    float rad = rand_uniform(-angle, angle) * TWO_PI / 360.;

    float dx = (im.w*scale/aspect - w) / 2.;
    float dy = (im.h*scale - w) / 2.;
    //if(dx < 0) dx = 0;
    //if(dy < 0) dy = 0;
    dx = rand_uniform(-dx, dx);
    dy = rand_uniform(-dy, dy);

    a.rad = rad;
    a.scale = scale;
    a.w = w;
    a.h = h;
    a.dx = dx;
    a.dy = dy;
    a.aspect = aspect;
    return a;
}

image random_augment_image(image im, float angle, float aspect, int low, int high, int w, int h)
{
    augment_args a = random_augment_args(im, angle, aspect, low, high, w, h);
    image crop = rotate_crop_image(im, a.rad, a.scale, a.w, a.h, a.dx, a.dy, a.aspect);
    return crop;
}

float three_way_max(float a, float b, float c)
{
    return (a > b) ? ( (a > c) ? a : c) : ( (b > c) ? b : c) ;
}

float three_way_min(float a, float b, float c)
{
    return (a < b) ? ( (a < c) ? a : c) : ( (b < c) ? b : c) ;
}

void yuv_to_rgb(image im)
{
    assert(im.c == 3);
    int i, j;
    float r, g, b;
    float y, u, v;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            y = get_pixel(im, i , j, 0);
            u = get_pixel(im, i , j, 1);
            v = get_pixel(im, i , j, 2);

            r = y + 1.13983*v;
            g = y + -.39465*u + -.58060*v;
            b = y + 2.03211*u;

            set_pixel(im, i, j, 0, r);
            set_pixel(im, i, j, 1, g);
            set_pixel(im, i, j, 2, b);
        }
    }
}

void rgb_to_yuv(image im)
{
    assert(im.c == 3);
    int i, j;
    float r, g, b;
    float y, u, v;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            r = get_pixel(im, i , j, 0);
            g = get_pixel(im, i , j, 1);
            b = get_pixel(im, i , j, 2);

            y = .299*r + .587*g + .114*b;
            u = -.14713*r + -.28886*g + .436*b;
            v = .615*r + -.51499*g + -.10001*b;

            set_pixel(im, i, j, 0, y);
            set_pixel(im, i, j, 1, u);
            set_pixel(im, i, j, 2, v);
        }
    }
}

// http://www.cs.rit.edu/~ncs/color/t_convert.html
void rgb_to_hsv(image im)
{
    assert(im.c == 3);
    int i, j;
    float r, g, b;
    float h, s, v;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            r = get_pixel(im, i , j, 0);
            g = get_pixel(im, i , j, 1);
            b = get_pixel(im, i , j, 2);
            float max = three_way_max(r,g,b);
            float min = three_way_min(r,g,b);
            float delta = max - min;
            v = max;
            if(max == 0){
                s = 0;
                h = 0;
            }else{
                s = delta/max;
                if(r == max){
                    h = (g - b) / delta;
                } else if (g == max) {
                    h = 2 + (b - r) / delta;
                } else {
                    h = 4 + (r - g) / delta;
                }
                if (h < 0) h += 6;
                h = h/6.;
            }
            set_pixel(im, i, j, 0, h);
            set_pixel(im, i, j, 1, s);
            set_pixel(im, i, j, 2, v);
        }
    }
}

void hsv_to_rgb(image im)
{
    assert(im.c == 3);
    int i, j;
    float r, g, b;
    float h, s, v;
    float f, p, q, t;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            h = 6 * get_pixel(im, i , j, 0);
            s = get_pixel(im, i , j, 1);
            v = get_pixel(im, i , j, 2);
            if (s == 0) {
                r = g = b = v;
            } else {
                int index = floor(h);
                f = h - index;
                p = v*(1-s);
                q = v*(1-s*f);
                t = v*(1-s*(1-f));
                if(index == 0){
                    r = v; g = t; b = p;
                } else if(index == 1){
                    r = q; g = v; b = p;
                } else if(index == 2){
                    r = p; g = v; b = t;
                } else if(index == 3){
                    r = p; g = q; b = v;
                } else if(index == 4){
                    r = t; g = p; b = v;
                } else {
                    r = v; g = p; b = q;
                }
            }
            set_pixel(im, i, j, 0, r);
            set_pixel(im, i, j, 1, g);
            set_pixel(im, i, j, 2, b);
        }
    }
}

void grayscale_image_3c(image im)
{
    assert(im.c == 3);
    int i, j, k;
    float scale[] = {0.299, 0.587, 0.114};
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            float val = 0;
            for(k = 0; k < 3; ++k){
                val += scale[k]*get_pixel(im, i, j, k);
            }
            im.data[0*im.h*im.w + im.w*j + i] = val;
            im.data[1*im.h*im.w + im.w*j + i] = val;
            im.data[2*im.h*im.w + im.w*j + i] = val;
        }
    }
}

image grayscale_image(image im)
{
    assert(im.c == 3);
    int i, j, k;
    image gray = make_image(im.w, im.h, 1);
    float scale[] = {0.299, 0.587, 0.114};
    for(k = 0; k < im.c; ++k){
        for(j = 0; j < im.h; ++j){
            for(i = 0; i < im.w; ++i){
                gray.data[i+im.w*j] += scale[k]*get_pixel(im, i, j, k);
            }
        }
    }
    return gray;
}

image threshold_image(image im, float thresh)
{
    int i;
    image t = make_image(im.w, im.h, im.c);
    for(i = 0; i < im.w*im.h*im.c; ++i){
        t.data[i] = im.data[i]>thresh ? 1 : 0;
    }
    return t;
}

image blend_image(image fore, image back, float alpha)
{
    assert(fore.w == back.w && fore.h == back.h && fore.c == back.c);
    image blend = make_image(fore.w, fore.h, fore.c);
    int i, j, k;
    for(k = 0; k < fore.c; ++k){
        for(j = 0; j < fore.h; ++j){
            for(i = 0; i < fore.w; ++i){
                float val = alpha * get_pixel(fore, i, j, k) +
                    (1 - alpha)* get_pixel(back, i, j, k);
                set_pixel(blend, i, j, k, val);
            }
        }
    }
    return blend;
}

void scale_image_channel(image im, int c, float v)
{
    int i, j;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            float pix = get_pixel(im, i, j, c);
            pix = pix*v;
            set_pixel(im, i, j, c, pix);
        }
    }
}

void translate_image_channel(image im, int c, float v)
{
    int i, j;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            float pix = get_pixel(im, i, j, c);
            pix = pix+v;
            set_pixel(im, i, j, c, pix);
        }
    }
}

image binarize_image(image im)
{
    image c = copy_image(im);
    int i;
    for(i = 0; i < im.w * im.h * im.c; ++i){
        if(c.data[i] > .5) c.data[i] = 1;
        else c.data[i] = 0;
    }
    return c;
}

void saturate_image(image im, float sat)
{
    rgb_to_hsv(im);
    scale_image_channel(im, 1, sat);
    hsv_to_rgb(im);
    constrain_image(im);
}

void hue_image(image im, float hue)
{
    rgb_to_hsv(im);
    int i;
    for(i = 0; i < im.w*im.h; ++i){
        im.data[i] = im.data[i] + hue;
        if (im.data[i] > 1) im.data[i] -= 1;
        if (im.data[i] < 0) im.data[i] += 1;
    }
    hsv_to_rgb(im);
    constrain_image(im);
}

void exposure_image(image im, float sat)
{
    rgb_to_hsv(im);
    scale_image_channel(im, 2, sat);
    hsv_to_rgb(im);
    constrain_image(im);
}

void distort_image(image im, float hue, float sat, float val)
{
    rgb_to_hsv(im);
    scale_image_channel(im, 1, sat);
    scale_image_channel(im, 2, val);
    int i;
    for(i = 0; i < im.w*im.h; ++i){
        im.data[i] = im.data[i] + hue;
        if (im.data[i] > 1) im.data[i] -= 1;
        if (im.data[i] < 0) im.data[i] += 1;
    }
    hsv_to_rgb(im);
    constrain_image(im);
}

void random_distort_image(image im, float hue, float saturation, float exposure)
{
    float dhue = rand_uniform(-hue, hue);
    float dsat = rand_scale(saturation);
    float dexp = rand_scale(exposure);
    distort_image(im, dhue, dsat, dexp);
}

void saturate_exposure_image(image im, float sat, float exposure)
{
    rgb_to_hsv(im);
    scale_image_channel(im, 1, sat);
    scale_image_channel(im, 2, exposure);
    hsv_to_rgb(im);
    constrain_image(im);
}

image resize_image(image im, int w, int h)
{
    image resized = make_image(w, h, im.c);
    image part = make_image(w, im.h, im.c);
    int r, c, k;
    float w_scale = (float)(im.w - 1) / (w - 1);
    float h_scale = (float)(im.h - 1) / (h - 1);
    for(k = 0; k < im.c; ++k){
        for(r = 0; r < im.h; ++r){
            for(c = 0; c < w; ++c){
                float val = 0;
                if(c == w-1 || im.w == 1){
                    val = get_pixel(im, im.w-1, r, k);
                } else {
                    float sx = c*w_scale;
                    int ix = (int) sx;
                    float dx = sx - ix;
                    val = (1 - dx) * get_pixel(im, ix, r, k) + dx * get_pixel(im, ix+1, r, k);
                }
                set_pixel(part, c, r, k, val);
            }
        }
    }
    for(k = 0; k < im.c; ++k){
        for(r = 0; r < h; ++r){
            float sy = r*h_scale;
            int iy = (int) sy;
            float dy = sy - iy;
            for(c = 0; c < w; ++c){
                float val = (1-dy) * get_pixel(part, c, iy, k);
                set_pixel(resized, c, r, k, val);
            }
            if(r == h-1 || im.h == 1) continue;
            for(c = 0; c < w; ++c){
                float val = dy * get_pixel(part, c, iy+1, k);
                add_pixel(resized, c, r, k, val);
            }
        }
    }

    free_image(part);
    return resized;
}


void test_resize(char *filename)
{
    image im = load_image(filename, 0,0, 3);
    float mag = mag_array(im.data, im.w*im.h*im.c);
    printf("L2 Norm: %f\n", mag);
    image gray = grayscale_image(im);

    image c1 = copy_image(im);
    image c2 = copy_image(im);
    image c3 = copy_image(im);
    image c4 = copy_image(im);
    distort_image(c1, .1, 1.5, 1.5);
    distort_image(c2, -.1, .66666, .66666);
    distort_image(c3, .1, 1.5, .66666);
    distort_image(c4, .1, .66666, 1.5);


    show_image(im,   "Original", 1);
    show_image(gray, "Gray", 1);
    show_image(c1, "C1", 1);
    show_image(c2, "C2", 1);
    show_image(c3, "C3", 1);
    show_image(c4, "C4", 1);
#ifdef OPENCV
    while(1){
        image aug = random_augment_image(im, 0, .75, 320, 448, 320, 320);
        show_image(aug, "aug", 1);
        free_image(aug);


        float exposure = 1.15;
        float saturation = 1.15;
        float hue = .05;

        image c = copy_image(im);

        float dexp = rand_scale(exposure);
        float dsat = rand_scale(saturation);
        float dhue = rand_uniform(-hue, hue);

        distort_image(c, dhue, dsat, dexp);
        show_image(c, "rand", 1);
        printf("%f %f %f\n", dhue, dsat, dexp);
        free_image(c);
    }
#endif
}


image load_image_stb(char *filename, int channels)
{
    int w, h, c;
    unsigned char *data = stbi_load(filename, &w, &h, &c, channels);
    if (!data) {
        fprintf(stderr, "Cannot load image \"%s\"\nSTB Reason: %s\n", filename, stbi_failure_reason());
        exit(0);
    }
    if(channels) c = channels;
    int i,j,k;
    image im = make_image(w, h, c);
    for(k = 0; k < c; ++k){
        for(j = 0; j < h; ++j){
            for(i = 0; i < w; ++i){
                int dst_index = i + w*j + w*h*k;
                int src_index = k + c*i + c*w*j;
                im.data[dst_index] = (float)data[src_index]/255.;
            }
        }
    }
    free(data);
    return im;
}

image load_image(char *filename, int w, int h, int c)
{
#ifdef OPENCV
    image out = load_image_cv(filename, c);
#else
    image out = load_image_stb(filename, c);
#endif

    if((h && w) && (h != out.h || w != out.w)){
        image resized = resize_image(out, w, h);
        free_image(out);
        out = resized;
    }
    return out;
}

image load_image_color(char *filename, int w, int h)
{
    return load_image(filename, w, h, 3);
}

image get_image_layer(image m, int l)
{
    image out = make_image(m.w, m.h, 1);
    int i;
    for(i = 0; i < m.h*m.w; ++i){
        out.data[i] = m.data[i+l*m.h*m.w];
    }
    return out;
}
void print_image(image m)
{
    int i, j, k;
    for(i =0 ; i < m.c; ++i){
        for(j =0 ; j < m.h; ++j){
            for(k = 0; k < m.w; ++k){
                printf("%.2lf, ", m.data[i*m.h*m.w + j*m.w + k]);
                if(k > 30) break;
            }
            printf("\n");
            if(j > 30) break;
        }
        printf("\n");
    }
    printf("\n");
}

image collapse_images_vert(image *ims, int n)
{
    int color = 1;
    int border = 1;
    int h,w,c;
    w = ims[0].w;
    h = (ims[0].h + border) * n - border;
    c = ims[0].c;
    if(c != 3 || !color){
        w = (w+border)*c - border;
        c = 1;
    }

    image filters = make_image(w, h, c);
    int i,j;
    for(i = 0; i < n; ++i){
        int h_offset = i*(ims[0].h+border);
        image copy = copy_image(ims[i]);
        //normalize_image(copy);
        if(c == 3 && color){
            embed_image(copy, filters, 0, h_offset);
        }
        else{
            for(j = 0; j < copy.c; ++j){
                int w_offset = j*(ims[0].w+border);
                image layer = get_image_layer(copy, j);
                embed_image(layer, filters, w_offset, h_offset);
                free_image(layer);
            }
        }
        free_image(copy);
    }
    return filters;
}

image collapse_images_horz(image *ims, int n)
{
    int color = 1;
    int border = 1;
    int h,w,c;
    int size = ims[0].h;
    h = size;
    w = (ims[0].w + border) * n - border;
    c = ims[0].c;
    if(c != 3 || !color){
        h = (h+border)*c - border;
        c = 1;
    }

    image filters = make_image(w, h, c);
    int i,j;
    for(i = 0; i < n; ++i){
        int w_offset = i*(size+border);
        image copy = copy_image(ims[i]);
        //normalize_image(copy);
        if(c == 3 && color){
            embed_image(copy, filters, w_offset, 0);
        }
        else{
            for(j = 0; j < copy.c; ++j){
                int h_offset = j*(size+border);
                image layer = get_image_layer(copy, j);
                embed_image(layer, filters, w_offset, h_offset);
                free_image(layer);
            }
        }
        free_image(copy);
    }
    return filters;
}

void show_image_normalized(image im, const char *name)
{
    image c = copy_image(im);
    normalize_image(c);
    show_image(c, name, 1);
    free_image(c);
}

void show_images(image *ims, int n, char *window)
{
    image m = collapse_images_vert(ims, n);
    /*
       int w = 448;
       int h = ((float)m.h/m.w) * 448;
       if(h > 896){
       h = 896;
       w = ((float)m.w/m.h) * 896;
       }
       image sized = resize_image(m, w, h);
     */
    normalize_image(m);
    save_image(m, window);
    show_image(m, window, 1);
    free_image(m);
}

void free_image(image m)
{
    if(m.data){
        free(m.data);
    }
}
