/*
 * Vector Rendering System for Typography
 * Pure C implementation - no external rendering libraries
 * Uses X11 only for pixel display (lowest level display interface)
 * 
 * Features:
 * - Cubic Bezier curves
 * - Scanline polygon filling
 * - Anti-aliasing via supersampling
 * - Vector font definitions

 * Use gcc -o render render.c -lX11 -lm -O2     
 * ./render
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ============================================================================
 * DISPLAY INTERFACE - X11 (minimal, just for putting pixels on screen)
 * ============================================================================ */
#include <X11/Xlib.h>

static Display *display;
static Window window;
static GC gc;
static XImage *ximage;

#define SCREEN_WIDTH  900
#define SCREEN_HEIGHT 700

/* ============================================================================
 * FRAMEBUFFER - Our own pixel buffer
 * ============================================================================ */
typedef struct {
    uint8_t r, g, b, a;
} Pixel;

static Pixel *framebuffer;

/* ============================================================================
 * VECTOR MATH TYPES
 * ============================================================================ */
typedef struct { float x, y; } Vec2;
typedef struct { float x, y, z; } Vec3;

static inline Vec2 vec2(float x, float y) {
    return (Vec2){x, y};
}

static inline Vec2 vec2_add(Vec2 a, Vec2 b) {
    return (Vec2){a.x + b.x, a.y + b.y};
}

static inline Vec2 vec2_sub(Vec2 a, Vec2 b) {
    return (Vec2){a.x - b.x, a.y - b.y};
}

static inline Vec2 vec2_scale(Vec2 v, float s) {
    return (Vec2){v.x * s, v.y * s};
}

static inline float vec2_dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}

static inline float vec2_cross(Vec2 a, Vec2 b) {
    return a.x * b.y - a.y * b.x;
}

static inline float vec2_length(Vec2 v) {
    return sqrtf(v.x * v.x + v.y * v.y);
}

static inline Vec2 vec2_normalize(Vec2 v) {
    float len = vec2_length(v);
    return len > 0 ? vec2_scale(v, 1.0f / len) : v;
}

static inline Vec2 vec2_perp(Vec2 v) {
    return (Vec2){-v.y, v.x};  /* 90 degree rotation */
}

/* ============================================================================
 * COLOR TYPES
 * ============================================================================ */
typedef struct { float r, g, b, a; } Color;

static inline Color color(float r, float g, float b, float a) {
    return (Color){r, g, b, a};
}

static inline Color color_lerp(Color a, Color b, float t) {
    return (Color){
        a.r + (b.r - a.r) * t,
        a.g + (b.g - a.g) * t,
        a.b + (b.b - a.b) * t,
        a.a + (b.a - a.a) * t
    };
}

/* Predefined colors */
#define COLOR_WHITE   ((Color){1,1,1,1})
#define COLOR_BLACK   ((Color){0,0,0,1})
#define COLOR_RED     ((Color){1,0,0,1})
#define COLOR_GREEN   ((Color){0,1,0,1})
#define COLOR_BLUE    ((Color){0,0,1,1})
#define COLOR_CYAN    ((Color){0,1,1,1})
#define COLOR_YELLOW  ((Color){1,1,0,1})
#define COLOR_GRAY    ((Color){0.5f,0.5f,0.5f,1})

/* ============================================================================
 * PATH DEFINITIONS - Vector shapes
 * ============================================================================ */
typedef enum {
    PATH_MOVE,
    PATH_LINE,
    PATH_QUADRATIC,  /* Quadratic Bezier */
    PATH_CUBIC,      /* Cubic Bezier */
    PATH_CLOSE
} PathCommandType;

typedef struct {
    PathCommandType type;
    Vec2 pts[4];  /* Points for command (max 4 for cubic bezier) */
} PathCommand;

typedef struct {
    PathCommand *commands;
    int count;
    int capacity;
} VectorPath;

/* Dynamic array for path commands */
static void path_init(VectorPath *path, int initial_capacity) {
    path->commands = malloc(sizeof(PathCommand) * initial_capacity);
    path->count = 0;
    path->capacity = initial_capacity;
}

static void path_free(VectorPath *path) {
    free(path->commands);
    path->commands = NULL;
    path->count = path->capacity = 0;
}

static void path_ensure_capacity(VectorPath *path) {
    if (path->count >= path->capacity) {
        path->capacity *= 2;
        path->commands = realloc(path->commands, sizeof(PathCommand) * path->capacity);
    }
}

static void path_move_to(VectorPath *path, Vec2 pt) {
    path_ensure_capacity(path);
    path->commands[path->count++] = (PathCommand){PATH_MOVE, {pt}};
}

static void path_line_to(VectorPath *path, Vec2 pt) {
    path_ensure_capacity(path);
    path->commands[path->count++] = (PathCommand){PATH_LINE, {pt}};
}

static void path_quadratic_to(VectorPath *path, Vec2 control, Vec2 end) {
    path_ensure_capacity(path);
    path->commands[path->count++] = (PathCommand){PATH_QUADRATIC, {control, end}};
}

static void path_cubic_to(VectorPath *path, Vec2 c1, Vec2 c2, Vec2 end) {
    path_ensure_capacity(path);
    path->commands[path->count++] = (PathCommand){PATH_CUBIC, {c1, c2, end}};
}

static void path_close(VectorPath *path) {
    path_ensure_capacity(path);
    path->commands[path->count++] = (PathCommand){PATH_CLOSE};
}

/* ============================================================================
 * BEZIER CURVE EVALUATION
 * ============================================================================ */

/* Quadratic Bezier: B(t) = (1-t)²P0 + 2(1-t)tP1 + t²P2 */
static Vec2 bezier_quadratic(Vec2 p0, Vec2 p1, Vec2 p2, float t) {
    float u = 1.0f - t;
    float u2 = u * u;
    float t2 = t * t;
    float ut2 = 2.0f * u * t;
    
    return (Vec2){
        u2 * p0.x + ut2 * p1.x + t2 * p2.x,
        u2 * p0.y + ut2 * p1.y + t2 * p2.y
    };
}

/* Cubic Bezier: B(t) = (1-t)³P0 + 3(1-t)²tP1 + 3(1-t)t²P2 + t³P3 */
static Vec2 bezier_cubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) {
    float u = 1.0f - t;
    float u2 = u * u;
    float u3 = u2 * u;
    float t2 = t * t;
    float t3 = t2 * t;
    
    return (Vec2){
        u3 * p0.x + 3.0f * u2 * t * p1.x + 3.0f * u * t2 * p2.x + t3 * p3.x,
        u3 * p0.y + 3.0f * u2 * t * p1.y + 3.0f * u * t2 * p2.y + t3 * p3.y
    };
}

/* Derivative of cubic Bezier (for computing tangent/normal) */
static Vec2 bezier_cubic_derivative(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) {
    float u = 1.0f - t;
    float u2 = u * u;
    float t2 = t * t;
    
    return (Vec2){
        3.0f * (u2 * (p1.x - p0.x) + 2.0f * u * t * (p2.x - p1.x) + t2 * (p3.x - p2.x)),
        3.0f * (u2 * (p1.y - p0.y) + 2.0f * u * t * (p2.y - p1.y) + t2 * (p3.y - p2.y))
    };
}

/* ============================================================================
 * PATH TESSELLATION - Convert curves to line segments
 * ============================================================================ */
typedef struct {
    Vec2 *points;
    int count;
    int capacity;
} Polyline;

static void polyline_init(Polyline *pl, int capacity) {
    pl->points = malloc(sizeof(Vec2) * capacity);
    pl->count = 0;
    pl->capacity = capacity;
}

static void polyline_free(Polyline *pl) {
    free(pl->points);
    pl->points = NULL;
    pl->count = pl->capacity = 0;
}

static void polyline_add(Polyline *pl, Vec2 pt) {
    if (pl->count >= pl->capacity) {
        pl->capacity *= 2;
        pl->points = realloc(pl->points, sizeof(Vec2) * pl->capacity);
    }
    pl->points[pl->count++] = pt;
}

/* Adaptive subdivision of cubic bezier based on flatness */
static void tessellate_cubic_bezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, 
                                     Polyline *out, float tolerance) {
    /* Check if curve is flat enough */
    Vec2 d1 = vec2_sub(p1, p0);
    Vec2 d2 = vec2_sub(p2, p1);
    Vec2 d3 = vec2_sub(p3, p2);
    
    float d1_len = vec2_length(d1);
    float d2_len = vec2_length(d2);
    float d3_len = vec2_length(d3);
    
    /* Estimate curve length */
    float estimated_len = d1_len + d2_len + d3_len;
    
    if (estimated_len < tolerance) {
        polyline_add(out, p3);
        return;
    }
    
    /* Subdivide at t=0.5 using de Casteljau's algorithm */
    Vec2 p01 = vec2_scale(vec2_add(p0, p1), 0.5f);
    Vec2 p12 = vec2_scale(vec2_add(p1, p2), 0.5f);
    Vec2 p23 = vec2_scale(vec2_add(p2, p3), 0.5f);
    Vec2 p012 = vec2_scale(vec2_add(p01, p12), 0.5f);
    Vec2 p123 = vec2_scale(vec2_add(p12, p23), 0.5f);
    Vec2 p0123 = vec2_scale(vec2_add(p012, p123), 0.5f);
    
    /* Recursively subdivide */
    tessellate_cubic_bezier(p0, p01, p012, p0123, out, tolerance);
    tessellate_cubic_bezier(p0123, p123, p23, p3, out, tolerance);
}

/* Convert path to polygon(s) */
static Polyline* path_to_polylines(VectorPath *path, int *out_count, float tolerance) {
    /* Count subpaths (for characters with multiple parts like 'i' or 'O') */
    int max_subpaths = 16;
    Polyline *polylines = malloc(sizeof(Polyline) * max_subpaths);
    int current_subpath = 0;
    polyline_init(&polylines[current_subpath], 256);
    
    Vec2 current = {0, 0};
    Vec2 start = {0, 0};
    
    for (int i = 0; i < path->count; i++) {
        PathCommand *cmd = &path->commands[i];
        
        switch (cmd->type) {
            case PATH_MOVE:
                if (polylines[current_subpath].count > 0) {
                    current_subpath++;
                    if (current_subpath >= max_subpaths) {
                        max_subpaths *= 2;
                        polylines = realloc(polylines, sizeof(Polyline) * max_subpaths);
                    }
                    polyline_init(&polylines[current_subpath], 256);
                }
                current = start = cmd->pts[0];
                break;
                
            case PATH_LINE:
                polyline_add(&polylines[current_subpath], cmd->pts[0]);
                current = cmd->pts[0];
                break;
                
            case PATH_QUADRATIC: {
                /* Convert quadratic to cubic */
                Vec2 p0 = current;
                Vec2 p1 = cmd->pts[0];
                Vec2 p2 = cmd->pts[1];
                Vec2 c1 = vec2_add(p0, vec2_scale(vec2_sub(p1, p0), 2.0f/3.0f));
                Vec2 c2 = vec2_add(p2, vec2_scale(vec2_sub(p1, p2), 2.0f/3.0f));
                
                polyline_add(&polylines[current_subpath], p0);
                tessellate_cubic_bezier(p0, c1, c2, p2, &polylines[current_subpath], tolerance);
                current = p2;
                break;
            }
            
            case PATH_CUBIC:
                polyline_add(&polylines[current_subpath], current);
                tessellate_cubic_bezier(current, cmd->pts[0], cmd->pts[1], cmd->pts[2],
                                        &polylines[current_subpath], tolerance);
                current = cmd->pts[2];
                break;
                
            case PATH_CLOSE:
                polyline_add(&polylines[current_subpath], start);
                current = start;
                break;
        }
    }
    
    *out_count = current_subpath + 1;
    return polylines;
}

/* ============================================================================
 * SCANLINE POLYGON FILLING - Rasterization
 * ============================================================================ */

/* Edge structure for scanline algorithm */
typedef struct {
    float y_max;
    float x_current;
    float x_step;  /* dx/dy */
} Edge;

/* Active edge table */
typedef struct {
    Edge *edges;
    int count;
    int capacity;
} EdgeTable;

static void edge_table_init(EdgeTable *et, int capacity) {
    et->edges = malloc(sizeof(Edge) * capacity);
    et->count = 0;
    et->capacity = capacity;
}

static void edge_table_free(EdgeTable *et) {
    free(et->edges);
}

static void edge_table_add(EdgeTable *et, Edge e) {
    if (et->count >= et->capacity) {
        et->capacity *= 2;
        et->edges = realloc(et->edges, sizeof(Edge) * et->capacity);
    }
    et->edges[et->count++] = e;
}

static int edge_compare(const void *a, const void *b) {
    float diff = ((Edge*)a)->x_current - ((Edge*)b)->x_current;
    return (diff < 0) ? -1 : (diff > 0) ? 1 : 0;
}

/* Fill a closed polygon using scanline algorithm */
static void fill_polygon(Vec2 *points, int count, Color color) {
    if (count < 3) return;
    
    /* Find bounding box */
    float y_min = points[0].y, y_max = points[0].y;
    for (int i = 1; i < count; i++) {
        if (points[i].y < y_min) y_min = points[i].y;
        if (points[i].y > y_max) y_max = points[i].y;
    }
    
    int scanline_start = (int)floorf(y_min);
    int scanline_end = (int)ceilf(y_max);
    
    EdgeTable global_edge_table[SCREEN_HEIGHT];
    for (int i = 0; i < SCREEN_HEIGHT; i++) {
        edge_table_init(&global_edge_table[i], 16);
    }
    
    /* Build global edge table */
    for (int i = 0; i < count; i++) {
        Vec2 p0 = points[i];
        Vec2 p1 = points[(i + 1) % count];
        
        /* Skip horizontal edges */
        if ((int)p0.y == (int)p1.y) continue;
        
        Edge e;
        e.y_max = fmaxf(p0.y, p1.y);
        e.x_current = (p0.y < p1.y) ? p0.x : p1.x;
        e.x_step = (p1.x - p0.x) / (p1.y - p0.y);
        
        int y_start = (int)floorf(fminf(p0.y, p1.y));
        if (y_start >= 0 && y_start < SCREEN_HEIGHT) {
            edge_table_add(&global_edge_table[y_start], e);
        }
    }
    
    /* Active edge table */
    EdgeTable aet;
    edge_table_init(&aet, 64);
    
    /* Process each scanline */
    for (int y = scanline_start; y <= scanline_end && y < SCREEN_HEIGHT; y++) {
        if (y < 0) continue;
        
        /* Add edges starting at this scanline */
        for (int i = 0; i < global_edge_table[y].count; i++) {
            edge_table_add(&aet, global_edge_table[y].edges[i]);
        }
        
        /* Sort active edges by x */
        qsort(aet.edges, aet.count, sizeof(Edge), edge_compare);
        
        /* Fill between pairs of edges */
        for (int i = 0; i < aet.count - 1; i += 2) {
            int x_start = (int)ceilf(aet.edges[i].x_current);
            int x_end = (int)floorf(aet.edges[i + 1].x_current);
            
            for (int x = x_start; x <= x_end && x < SCREEN_WIDTH; x++) {
                if (x >= 0) {
                    /* Blend with existing pixel */
                    Pixel *p = &framebuffer[y * SCREEN_WIDTH + x];
                    p->r = (uint8_t)(color.r * 255);
                    p->g = (uint8_t)(color.g * 255);
                    p->b = (uint8_t)(color.b * 255);
                    p->a = 255;
                }
            }
        }
        
        /* Remove edges that end at this scanline and update x values */
        int write_idx = 0;
        for (int i = 0; i < aet.count; i++) {
            if ((int)aet.edges[i].y_max > y + 1) {
                aet.edges[i].x_current += aet.edges[i].x_step;
                aet.edges[write_idx++] = aet.edges[i];
            }
        }
        aet.count = write_idx;
    }
    
    /* Cleanup */
    for (int i = 0; i < SCREEN_HEIGHT; i++) {
        edge_table_free(&global_edge_table[i]);
    }
    edge_table_free(&aet);
}

/* ============================================================================
 * STROKE RENDERING - Draw outlines
 * ============================================================================ */

/* Draw a single pixel */
static void draw_pixel(int x, int y, Color color) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= SCREEN_HEIGHT) return;
    
    Pixel *p = &framebuffer[y * SCREEN_WIDTH + x];
    float alpha = color.a;
    float inv_alpha = 1.0f - alpha;
    
    p->r = (uint8_t)(color.r * 255 * alpha + p->r * inv_alpha);
    p->g = (uint8_t)(color.g * 255 * alpha + p->g * inv_alpha);
    p->b = (uint8_t)(color.b * 255 * alpha + p->b * inv_alpha);
}

/* Draw a filled circle (for round line caps) */
static void fill_circle(Vec2 center, float radius, Color color) {
    int x_start = (int)(center.x - radius - 1);
    int x_end = (int)(center.x + radius + 1);
    int y_start = (int)(center.y - radius - 1);
    int y_end = (int)(center.y + radius + 1);
    
    float r2 = radius * radius;
    
    for (int y = y_start; y <= y_end; y++) {
        for (int x = x_start; x <= x_end; x++) {
            float dx = x - center.x;
            float dy = y - center.y;
            if (dx * dx + dy * dy <= r2) {
                draw_pixel(x, y, color);
            }
        }
    }
}

/* Draw line between two points with thickness */
static void draw_thick_line(Vec2 p0, Vec2 p1, float thickness, Color color) {
    Vec2 dir = vec2_sub(p1, p0);
    float len = vec2_length(dir);
    if (len < 0.001f) {
        fill_circle(p0, thickness * 0.5f, color);
        return;
    }
    
    dir = vec2_scale(dir, 1.0f / len);
    Vec2 perp = vec2_perp(dir);
    float half_thickness = thickness * 0.5f;
    
    /* Build quad vertices */
    Vec2 quad[4] = {
        vec2_add(p0, vec2_scale(perp, half_thickness)),
        vec2_sub(p0, vec2_scale(perp, half_thickness)),
        vec2_sub(p1, vec2_scale(perp, half_thickness)),
        vec2_add(p1, vec2_scale(perp, half_thickness))
    };
    
    fill_polygon(quad, 4, color);
}

/* Stroke a polyline with thickness */
static void stroke_polyline(Vec2 *points, int count, float thickness, Color color) {
    if (count < 2) {
        if (count == 1) {
            fill_circle(points[0], thickness * 0.5f, color);
        }
        return;
    }
    
    /* Draw line segments */
    for (int i = 0; i < count - 1; i++) {
        draw_thick_line(points[i], points[i + 1], thickness, color);
    }
    
    /* Draw round caps */
    fill_circle(points[0], thickness * 0.5f, color);
    fill_circle(points[count - 1], thickness * 0.5f, color);
}

/* ============================================================================
 * VECTOR FONT DEFINITIONS
 * ============================================================================ */

/* Font metrics */
#define FONT_EM_SIZE 100.0f
#define FONT_ASCENT  80.0f
#define FONT_DESCENT 20.0f

/* Define letter 'A' */
static void define_letter_A(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    path_move_to(path, vec2(ox + 0*s, oy + 0*s));
    path_line_to(path, vec2(ox + 50*s, oy + 100*s));
    path_line_to(path, vec2(ox + 100*s, oy + 0*s));
    path_move_to(path, vec2(ox + 20*s, oy + 35*s));
    path_line_to(path, vec2(ox + 80*s, oy + 35*s));
    path_close(path);
}

/* Define letter 'B' */
static void define_letter_B(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    /* Main body */
    path_move_to(path, vec2(ox + 15*s, oy + 0*s));
    path_line_to(path, vec2(ox + 15*s, oy + 100*s));
    path_line_to(path, vec2(ox + 60*s, oy + 100*s));
    
    /* Top curve */
    path_cubic_to(path, 
        vec2(ox + 95*s, oy + 100*s),
        vec2(ox + 100*s, oy + 75*s),
        vec2(ox + 85*s, oy + 55*s)
    );
    
    /* Middle connector */
    path_line_to(path, vec2(ox + 50*s, oy + 52*s));
    
    /* Bottom curve */
    path_cubic_to(path,
        vec2(ox + 105*s, oy + 50*s),
        vec2(ox + 105*s, oy + 10*s),
        vec2(ox + 65*s, oy + 0*s)
    );
    path_close(path);
    
    /* Inner counter (hole) */
    path_move_to(path, vec2(ox + 30*s, oy + 55*s));
    path_line_to(path, vec2(ox + 30*s, oy + 90*s));
    path_line_to(path, vec2(ox + 55*s, oy + 90*s));
    path_cubic_to(path,
        vec2(ox + 75*s, oy + 90*s),
        vec2(ox + 78*s, oy + 70*s),
        vec2(ox + 68*s, oy + 58*s)
    );
    path_close(path);
}

/* Define letter 'C' */
static void define_letter_C(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    path_move_to(path, vec2(ox + 95*s, oy + 80*s));
    path_cubic_to(path,
        vec2(ox + 85*s, oy + 95*s),
        vec2(ox + 65*s, oy + 105*s),
        vec2(ox + 45*s, oy + 105*s)
    );
    path_cubic_to(path,
        vec2(ox + 5*s, oy + 105*s),
        vec2(ox + -5*s, oy + 60*s),
        vec2(ox + 10*s, oy + 30*s)
    );
    path_cubic_to(path,
        vec2(ox + 20*s, oy + -5*s),
        vec2(ox + 60*s, oy + -5*s),
        vec2(ox + 90*s, oy + 15*s)
    );
    path_line_to(path, vec2(ox + 80*s, oy + 35*s));
    path_cubic_to(path,
        vec2(ox + 60*s, oy + 20*s),
        vec2(ox + 35*s, oy + 20*s),
        vec2(ox + 25*s, oy + 40*s)
    );
    path_cubic_to(path,
        vec2(ox + 12*s, oy + 65*s),
        vec2(ox + 20*s, oy + 90*s),
        vec2(ox + 50*s, oy + 85*s)
    );
    path_cubic_to(path,
        vec2(ox + 70*s, oy + 82*s),
        vec2(ox + 85*s, oy + 70*s),
        vec2(ox + 95*s, oy + 60*s)
    );
    path_close(path);
}

/* Define letter 'O' */
static void define_letter_O(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    /* Outer contour */
    path_move_to(path, vec2(ox + 55*s, oy + 0*s));
    path_cubic_to(path,
        vec2(ox + 100*s, oy + 0*s),
        vec2(ox + 110*s, oy + 50*s),
        vec2(ox + 110*s, oy + 50*s)
    );
    path_cubic_to(path,
        vec2(ox + 110*s, oy + 100*s),
        vec2(ox + 55*s, oy + 110*s),
        vec2(ox + 55*s, oy + 110*s)
    );
    path_cubic_to(path,
        vec2(ox + 0*s, oy + 110*s),
        vec2(ox + -5*s, oy + 55*s),
        vec2(ox + -5*s, oy + 55*s)
    );
    path_cubic_to(path,
        vec2(ox + -5*s, oy + 0*s),
        vec2(ox + 55*s, oy + 0*s),
        vec2(ox + 55*s, oy + 0*s)
    );
    path_close(path);
    
    /* Inner contour (hole) */
    path_move_to(path, vec2(ox + 55*s, oy + 25*s));
    path_cubic_to(path,
        vec2(ox + 80*s, oy + 25*s),
        vec2(ox + 85*s, oy + 55*s),
        vec2(ox + 85*s, oy + 55*s)
    );
    path_cubic_to(path,
        vec2(ox + 85*s, oy + 85*s),
        vec2(ox + 55*s, oy + 85*s),
        vec2(ox + 55*s, oy + 85*s)
    );
    path_cubic_to(path,
        vec2(ox + 25*s, oy + 85*s),
        vec2(ox + 20*s, oy + 55*s),
        vec2(ox + 20*s, oy + 55*s)
    );
    path_cubic_to(path,
        vec2(ox + 20*s, oy + 25*s),
        vec2(ox + 55*s, oy + 25*s),
        vec2(ox + 55*s, oy + 25*s)
    );
    path_close(path);
}

/* Define letter 'P' */
static void define_letter_P(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    /* Stem */
    path_move_to(path, vec2(ox + 15*s, oy + 0*s));
    path_line_to(path, vec2(ox + 15*s, oy + 100*s));
    path_line_to(path, vec2(ox + 55*s, oy + 100*s));
    
    /* Bowl */
    path_cubic_to(path,
        vec2(ox + 105*s, oy + 100*s),
        vec2(ox + 105*s, oy + 45*s),
        vec2(ox + 55*s, oy + 45*s)
    );
    path_line_to(path, vec2(ox + 35*s, oy + 45*s));
    path_line_to(path, vec2(ox + 35*s, oy + 0*s));
    path_close(path);
    
    /* Counter (hole) */
    path_move_to(path, vec2(ox + 35*s, oy + 60*s));
    path_line_to(path, vec2(ox + 55*s, oy + 60*s));
    path_cubic_to(path,
        vec2(ox + 80*s, oy + 60*s),
        vec2(ox + 80*s, oy + 85*s),
        vec2(ox + 55*s, oy + 85*s)
    );
    path_line_to(path, vec2(ox + 35*s, oy + 85*s));
    path_close(path);
}

/* Define letter 'R' */
static void define_letter_R(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    /* Stem */
    path_move_to(path, vec2(ox + 15*s, oy + 0*s));
    path_line_to(path, vec2(ox + 15*s, oy + 100*s));
    path_line_to(path, vec2(ox + 55*s, oy + 100*s));
    
    /* Bowl */
    path_cubic_to(path,
        vec2(ox + 100*s, oy + 100*s),
        vec2(ox + 100*s, oy + 50*s),
        vec2(ox + 55*s, oy + 50*s)
    );
    path_line_to(path, vec2(ox + 35*s, oy + 50*s));
    
    /* Leg */
    path_line_to(path, vec2(ox + 70*s, oy + 0*s));
    path_line_to(path, vec2(ox + 50*s, oy + 0*s));
    path_line_to(path, vec2(ox + 35*s, oy + 45*s));
    path_line_to(path, vec2(ox + 35*s, oy + 0*s));
    path_close(path);
}

/* Define letter 'S' */
static void define_letter_S(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    path_move_to(path, vec2(ox + 20*s, oy + 20*s));
    path_cubic_to(path,
        vec2(ox + 20*s, oy + 5*s),
        vec2(ox + 40*s, oy + -5*s),
        vec2(ox + 60*s, oy + 0*s)
    );
    path_cubic_to(path,
        vec2(ox + 95*s, oy + 5*s),
        vec2(ox + 95*s, oy + 40*s),
        vec2(ox + 60*s, oy + 50*s)
    );
    path_line_to(path, vec2(ox + 45*s, oy + 55*s));
    path_cubic_to(path,
        vec2(ox + 15*s, oy + 60*s),
        vec2(ox + 15*s, oy + 95*s),
        vec2(ox + 50*s, oy + 105*s)
    );
    path_cubic_to(path,
        vec2(ox + 75*s, oy + 110*s),
        vec2(ox + 95*s, oy + 100*s),
        vec2(ox + 95*s, oy + 80*s)
    );
    path_line_to(path, vec2(ox + 75*s, oy + 80*s));
    path_cubic_to(path,
        vec2(ox + 75*s, oy + 92*s),
        vec2(ox + 60*s, oy + 95*s),
        vec2(ox + 50*s, oy + 90*s)
    );
    path_cubic_to(path,
        vec2(ox + 35*s, oy + 85*s),
        vec2(ox + 35*s, oy + 70*s),
        vec2(ox + 55*s, oy + 65*s)
    );
    path_line_to(path, vec2(ox + 70*s, oy + 60*s));
    path_cubic_to(path,
        vec2(ox + 110*s, oy + 50*s),
        vec2(ox + 110*s, oy + 10*s),
        vec2(ox + 65*s, oy + 0*s)
    );
    path_cubic_to(path,
        vec2(ox + 35*s, oy + -5*s),
        vec2(ox + 5*s, oy + 5*s),
        vec2(ox + 5*s, oy + 30*s)
    );
    path_close(path);
}

/* Define letter 'V' */
static void define_letter_V(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    path_move_to(path, vec2(ox + 0*s, oy + 0*s));
    path_line_to(path, vec2(ox + 50*s, oy + 100*s));
    path_line_to(path, vec2(ox + 100*s, oy + 0*s));
    path_line_to(path, vec2(ox + 80*s, oy + 0*s));
    path_line_to(path, vec2(ox + 50*s, oy + 70*s));
    path_line_to(path, vec2(ox + 20*s, oy + 0*s));
    path_close(path);
}

/* Define letter 'i' */
static void define_letter_i(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    /* Stem */
    path_move_to(path, vec2(ox + 35*s, oy + 25*s));
    path_line_to(path, vec2(ox + 35*s, oy + 100*s));
    path_line_to(path, vec2(ox + 55*s, oy + 100*s));
    path_line_to(path, vec2(ox + 55*s, oy + 25*s));
    path_close(path);
    
    /* Dot */
    path_move_to(path, vec2(ox + 45*s, oy + 0*s));
    path_cubic_to(path,
        vec2(ox + 55*s, oy + 0*s),
        vec2(ox + 60*s, oy + 10*s),
        vec2(ox + 60*s, oy + 15*s)
    );
    path_cubic_to(path,
        vec2(ox + 60*s, oy + 22*s),
        vec2(ox + 53*s, oy + 25*s),
        vec2(ox + 45*s, oy + 25*s)
    );
    path_cubic_to(path,
        vec2(ox + 37*s, oy + 25*s),
        vec2(ox + 30*s, oy + 22*s),
        vec2(ox + 30*s, oy + 15*s)
    );
    path_cubic_to(path,
        vec2(ox + 30*s, oy + 10*s),
        vec2(ox + 35*s, oy + 0*s),
        vec2(ox + 45*s, oy + 0*s)
    );
    path_close(path);
}

/* Define letter 'e' */
static void define_letter_e(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    path_move_to(path, vec2(ox + 55*s, oy + 0*s));
    path_cubic_to(path,
        vec2(ox + 15*s, oy + 0*s),
        vec2(ox + -5*s, oy + 30*s),
        vec2(ox + -5*s, oy + 55*s)
    );
    path_cubic_to(path,
        vec2(ox + -5*s, oy + 90*s),
        vec2(ox + 30*s, oy + 105*s),
        vec2(ox + 55*s, oy + 105*s)
    );
    path_cubic_to(path,
        vec2(ox + 85*s, oy + 105*s),
        vec2(ox + 100*s, oy + 85*s),
        vec2(ox + 100*s, oy + 60*s)
    );
    path_line_to(path, vec2(ox + 20*s, oy + 60*s));
    path_cubic_to(path,
        vec2(ox + 22*s, oy + 35*s),
        vec2(ox + 40*s, oy + 20*s),
        vec2(ox + 60*s, oy + 20*s)
    );
    path_cubic_to(path,
        vec2(ox + 80*s, oy + 20*s),
        vec2(ox + 90*s, oy + 30*s),
        vec2(ox + 95*s, oy + 45*s)
    );
    path_line_to(path, vec2(ox + 100*s, oy + 35*s));
    path_cubic_to(path,
        vec2(ox + 90*s, oy + 10*s),
        vec2(ox + 75*s, oy + 0*s),
        vec2(ox + 55*s, oy + 0*s)
    );
    path_close(path);
    
    /* Counter */
    path_move_to(path, vec2(ox + 20*s, oy + 75*s));
    path_line_to(path, vec2(ox + 80*s, oy + 75*s));
    path_cubic_to(path,
        vec2(ox + 75*s, oy + 90*s),
        vec2(ox + 65*s, oy + 88*s),
        vec2(ox + 55*s, oy + 88*s)
    );
    path_cubic_to(path,
        vec2(ox + 35*s, oy + 88*s),
        vec2(ox + 22*s, oy + 82*s),
        vec2(ox + 20*s, oy + 75*s)
    );
    path_close(path);
}

/* Define letter 'a' */
static void define_letter_a(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    /* Bowl */
    path_move_to(path, vec2(ox + 75*s, oy + 0*s));
    path_line_to(path, vec2(ox + 75*s, oy + 15*s));
    path_cubic_to(path,
        vec2(ox + 60*s, oy + -5*s),
        vec2(ox + 30*s, oy + -5*s),
        vec2(ox + 20*s, oy + 20*s)
    );
    path_cubic_to(path,
        vec2(ox + 5*s, oy + 50*s),
        vec2(ox + 20*s, oy + 80*s),
        vec2(ox + 50*s, oy + 80*s)
    );
    path_cubic_to(path,
        vec2(ox + 70*s, oy + 80*s),
        vec2(ox + 78*s, oy + 65*s),
        vec2(ox + 80*s, oy + 55*s)
    );
    path_line_to(path, vec2(ox + 80*s, oy + 100*s));
    path_line_to(path, vec2(ox + 100*s, oy + 100*s));
    path_line_to(path, vec2(ox + 100*s, oy + 0*s));
    path_close(path);
    
    /* Counter */
    path_move_to(path, vec2(ox + 25*s, oy + 40*s));
    path_cubic_to(path,
        vec2(ox + 25*s, oy + 20*s),
        vec2(ox + 55*s, oy + 15*s),
        vec2(ox + 75*s, oy + 40*s)
    );
    path_line_to(path, vec2(ox + 75*s, oy + 45*s));
    path_cubic_to(path,
        vec2(ox + 55*s, oy + 65*s),
        vec2(ox + 25*s, oy + 60*s),
        vec2(ox + 25*s, oy + 40*s)
    );
    path_close(path);
}

/* Define letter 'o' (lowercase) */
static void define_letter_o(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    /* Outer */
    path_move_to(path, vec2(ox + 50*s, oy + 0*s));
    path_cubic_to(path,
        vec2(ox + 10*s, oy + 0*s),
        vec2(ox + -5*s, oy + 40*s),
        vec2(ox + -5*s, oy + 50*s)
    );
    path_cubic_to(path,
        vec2(ox + -5*s, oy + 80*s),
        vec2(ox + 25*s, oy + 105*s),
        vec2(ox + 50*s, oy + 105*s)
    );
    path_cubic_to(path,
        vec2(ox + 80*s, oy + 105*s),
        vec2(ox + 105*s, oy + 75*s),
        vec2(ox + 105*s, oy + 50*s)
    );
    path_cubic_to(path,
        vec2(ox + 105*s, oy + 15*s),
        vec2(ox + 80*s, oy + 0*s),
        vec2(ox + 50*s, oy + 0*s)
    );
    path_close(path);
    
    /* Inner (counter) */
    path_move_to(path, vec2(ox + 50*s, oy + 25*s));
    path_cubic_to(path,
        vec2(ox + 70*s, oy + 25*s),
        vec2(ox + 80*s, oy + 40*s),
        vec2(ox + 80*s, oy + 50*s)
    );
    path_cubic_to(path,
        vec2(ox + 80*s, oy + 65*s),
        vec2(ox + 65*s, oy + 80*s),
        vec2(ox + 50*s, oy + 80*s)
    );
    path_cubic_to(path,
        vec2(ox + 35*s, oy + 80*s),
        vec2(ox + 20*s, oy + 65*s),
        vec2(ox + 20*s, oy + 50*s)
    );
    path_cubic_to(path,
        vec2(ox + 20*s, oy + 35*s),
        vec2(ox + 30*s, oy + 25*s),
        vec2(ox + 50*s, oy + 25*s)
    );
    path_close(path);
}

/* Define letter 'r' */
static void define_letter_r(VectorPath *path, float x_offset, float y_offset, float scale) {
    float s = scale;
    float ox = x_offset;
    float oy = y_offset;
    
    path_move_to(path, vec2(ox + 15*s, oy + 0*s));
    path_line_to(path, vec2(ox + 15*s, oy + 80*s));
    path_line_to(path, vec2(ox + 35*s, oy + 80*s));
    path_line_to(path, vec2(ox + 35*s, oy + 60*s));
    path_cubic_to(path,
        vec2(ox + 40*s, oy + 75*s),
        vec2(ox + 55*s, oy + 85*s),
        vec2(ox + 70*s, oy + 80*s)
    );
    path_line_to(path, vec2(ox + 70*s, oy + 60*s));
    path_cubic_to(path,
        vec2(ox + 50*s, oy + 65*s),
        vec2(ox + 35*s, oy + 50*s),
        vec2(ox + 35*s, oy + 30*s)
    );
    path_line_to(path, vec2(ox + 35*s, oy + 0*s));
    path_close(path);
}

/* ============================================================================
 * TRANSFORMATIONS
 * ============================================================================ */

typedef struct {
    float m[3][3];
} Matrix3x3;

static Matrix3x3 matrix_identity(void) {
    Matrix3x3 m = {{{1,0,0}, {0,1,0}, {0,0,1}}};
    return m;
}

static Matrix3x3 matrix_translation(float tx, float ty) {
    Matrix3x3 m = {{{1,0,tx}, {0,1,ty}, {0,0,1}}};
    return m;
}

static Matrix3x3 matrix_scale(float sx, float sy) {
    Matrix3x3 m = {{{sx,0,0}, {0,sy,0}, {0,0,1}}};
    return m;
}

static Matrix3x3 matrix_rotation(float angle) {
    float c = cosf(angle);
    float s = sinf(angle);
    Matrix3x3 m = {{{c,-s,0}, {s,c,0}, {0,0,1}}};
    return m;
}

static Matrix3x3 matrix_multiply(Matrix3x3 a, Matrix3x3 b) {
    Matrix3x3 result = {0};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            for (int k = 0; k < 3; k++) {
                result.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }
    return result;
}

static Vec2 matrix_transform(Matrix3x3 m, Vec2 v) {
    float x = m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2];
    float y = m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2];
    return (Vec2){x, y};
}

static void transform_path(VectorPath *path, Matrix3x3 m) {
    for (int i = 0; i < path->count; i++) {
        for (int j = 0; j < 4; j++) {
            path->commands[i].pts[j] = matrix_transform(m, path->commands[i].pts[j]);
        }
    }
}

/* ============================================================================
 * RENDERING FUNCTIONS
 * ============================================================================ */

typedef enum {
    FILL_RULE_NONZERO,
    FILL_RULE_EVENODD
} FillRule;

/* Render a vector path */
static void render_path(VectorPath *path, Color fill_color, Color stroke_color, 
                        float stroke_width, FillRule fill_rule) {
    int polyline_count;
    Polyline *polylines = path_to_polylines(path, &polyline_count, 0.5f);
    
    /* Fill each subpath */
    for (int i = 0; i < polyline_count; i++) {
        Polyline *pl = &polylines[i];
        if (pl->count > 2) {
            fill_polygon(pl->points, pl->count, fill_color);
        }
    }
    
    /* Stroke each subpath */
    if (stroke_width > 0) {
        for (int i = 0; i < polyline_count; i++) {
            Polyline *pl = &polylines[i];
            if (pl->count > 1) {
                stroke_polyline(pl->points, pl->count, stroke_width, stroke_color);
            }
        }
    }
    
    /* Cleanup */
    for (int i = 0; i < polyline_count; i++) {
        polyline_free(&polylines[i]);
    }
    free(polylines);
}

/* ============================================================================
 * DISPLAY INITIALIZATION
 * ============================================================================ */

static int init_display(void) {
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "Cannot open X display\n");
        return 0;
    }
    
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    
    window = XCreateSimpleWindow(display, root, 0, 0, 
                                  SCREEN_WIDTH, SCREEN_HEIGHT, 0,
                                  BlackPixel(display, screen),
                                  BlackPixel(display, screen));
    
    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);
    
    gc = XCreateGC(display, window, 0, NULL);
    
    /* Create framebuffer */
    framebuffer = calloc(SCREEN_WIDTH * SCREEN_HEIGHT, sizeof(Pixel));
    
    /* Create XImage for fast blitting */
    ximage = XCreateImage(display, DefaultVisual(display, screen),
                          24, ZPixmap, 0, (char*)framebuffer,
                          SCREEN_WIDTH, SCREEN_HEIGHT, 32, 0);
    
    return 1;
}

static void close_display(void) {
    XDestroyImage(ximage);
    free(framebuffer);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
}

static void clear_framebuffer(Color color) {
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        framebuffer[i].r = (uint8_t)(color.r * 255);
        framebuffer[i].g = (uint8_t)(color.g * 255);
        framebuffer[i].b = (uint8_t)(color.b * 255);
        framebuffer[i].a = 255;
    }
}

static void update_display(void) {
    XPutImage(display, window, gc, ximage, 0, 0, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
}

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

/* Draw a grid for reference */
static void draw_grid(Color color, int spacing) {
    for (int x = 0; x < SCREEN_WIDTH; x += spacing) {
        for (int y = 0; y < SCREEN_HEIGHT; y++) {
            draw_pixel(x, y, color);
        }
    }
    for (int y = 0; y < SCREEN_HEIGHT; y += spacing) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            draw_pixel(x, y, color);
        }
    }
}

/* Draw coordinate axes */
static void draw_axes(void) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        draw_pixel(x, SCREEN_HEIGHT/2, (Color){0.3f, 0.3f, 0.3f, 1});
    }
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        draw_pixel(SCREEN_WIDTH/2, y, (Color){0.3f, 0.3f, 0.3f, 1});
    }
}

/* ============================================================================
 * MAIN APPLICATION
 * ============================================================================ */

typedef void (*LetterDefinition)(VectorPath*, float, float, float);

typedef struct {
    char character;
    LetterDefinition define;
    float advance_width;
} GlyphInfo;

static GlyphInfo glyph_table[] = {
    {'A', define_letter_A, 100},
    {'B', define_letter_B, 90},
    {'C', define_letter_C, 85},
    {'O', define_letter_O, 95},
    {'P', define_letter_P, 80},
    {'R', define_letter_R, 85},
    {'S', define_letter_S, 80},
    {'V', define_letter_V, 90},
    {'a', define_letter_a, 85},
    {'e', define_letter_e, 85},
    {'i', define_letter_i, 40},
    {'o', define_letter_o, 85},
    {'r', define_letter_r, 55},
    {0, NULL, 0}
};

static GlyphInfo* find_glyph(char c) {
    for (int i = 0; glyph_table[i].define != NULL; i++) {
        if (glyph_table[i].character == c) {
            return &glyph_table[i];
        }
    }
    return NULL;
}

/* Render a string of text */
static void render_text(const char *text, float x, float y, float scale, 
                        Color fill, Color stroke, float stroke_width) {
    float cursor_x = x;
    
    for (const char *p = text; *p; p++) {
        if (*p == ' ') {
            cursor_x += 40 * scale;
            continue;
        }
        
        GlyphInfo *glyph = find_glyph(*p);
        if (glyph) {
            VectorPath path;
            path_init(&path, 64);
            
            glyph->define(&path, cursor_x, y, scale);
            
            render_path(&path, fill, stroke, stroke_width, FILL_RULE_NONZERO);
            
            path_free(&path);
            cursor_x += glyph->advance_width * scale;
        }
    }
}

/* Draw a decorative border */
static void draw_border_frame(void) {
    VectorPath frame;
    path_init(&frame, 64);
    
    /* Outer frame */
    path_move_to(&frame, vec2(20, 20));
    path_line_to(&frame, vec2(SCREEN_WIDTH - 20, 20));
    path_line_to(&frame, vec2(SCREEN_WIDTH - 20, SCREEN_HEIGHT - 20));
    path_line_to(&frame, vec2(20, SCREEN_HEIGHT - 20));
    path_close(&frame);
    
    /* Inner cutout */
    path_move_to(&frame, vec2(40, 40));
    path_line_to(&frame, vec2(40, SCREEN_HEIGHT - 40));
    path_line_to(&frame, vec2(SCREEN_WIDTH - 40, SCREEN_HEIGHT - 40));
    path_line_to(&frame, vec2(SCREEN_WIDTH - 40, 40));
    path_close(&frame);
    
    stroke_polyline(frame.commands[0].pts, 4, 3.0f, (Color){0.4f, 0.6f, 0.8f, 1});
    
    path_free(&frame);
}

/* Draw bezier curve demonstration */
static void draw_bezier_demo(float ox, float oy) {
    Vec2 p0 = {ox, oy + 80};
    Vec2 p1 = {ox + 50, oy - 20};
    Vec2 p2 = {ox + 100, oy + 120};
    Vec2 p3 = {ox + 150, oy + 40};
    
    /* Draw control polygon */
    stroke_polyline((Vec2[]){p0, p1, p2, p3}, 4, 1.0f, (Color){0.5f, 0.5f, 0.5f, 0.5f});
    
    /* Draw control points */
    fill_circle(p0, 5, (Color){1, 0, 0, 1});
    fill_circle(p1, 5, (Color){0, 1, 0, 1});
    fill_circle(p2, 5, (Color){0, 1, 0, 1});
    fill_circle(p3, 5, (Color){0, 0, 1, 1});
    
    /* Draw the bezier curve */
    Polyline curve;
    polyline_init(&curve, 256);
    polyline_add(&curve, p0);
    tessellate_cubic_bezier(p0, p1, p2, p3, &curve, 1.0f);
    stroke_polyline(curve.points, curve.count, 3.0f, (Color){1, 1, 1, 1});
    polyline_free(&curve);
}

int main(void) {
    printf("Vector Typography Renderer\n");
    printf("==========================\n\n");
    printf("This system implements:\n");
    printf("  - Cubic Bezier curves with adaptive tessellation\n");
    printf("  - Scanline polygon filling algorithm\n");
    printf("  - Vector font definitions\n");
    printf("  - Path stroking with thickness\n\n");
    printf("Press any key to exit.\n\n");
    
    if (!init_display()) {
        return 1;
    }
    
    int running = 1;
    float time = 0;
    
    while (running) {
        /* Clear to dark background */
        clear_framebuffer((Color){0.05f, 0.05f, 0.1f, 1});
        
        /* Draw subtle grid */
        draw_grid((Color){0.1f, 0.1f, 0.15f, 0.5f}, 50);
        
        /* Title */
        render_text("Vector Typography", 60, 80, 1.2f, 
                   (Color){0.9f, 0.9f, 0.95f, 1}, (Color){0, 0, 0, 0}, 0);
        
        /* Subtitle */
        render_text("Pure C Rendering", 60, 220, 0.8f, 
                   (Color){0.4f, 0.7f, 1.0f, 1}, (Color){0, 0, 0, 0}, 0);
        
        /* Sample letters with different styles */
        render_text("ABC", 60, 350, 1.5f, 
                   (Color){1.0f, 0.4f, 0.4f, 1}, (Color){1, 1, 1, 0.3f}, 2);
        
        render_text("over", 350, 350, 1.5f, 
                   (Color){0.4f, 1.0f, 0.4f, 1}, (Color){0, 0, 0, 0}, 0);
        
        /* Lowercase sample */
        render_text("a e i o r", 60, 500, 1.3f, 
                   (Color){0.9f, 0.8f, 0.4f, 1}, (Color){0.2f, 0.2f, 0.2f, 1}, 1);
        
        /* Animated rotation demo */
        float angle = sinf(time * 0.5f) * 0.2f;
        Matrix3x3 transform = matrix_multiply(
            matrix_translation(750, 250),
            matrix_multiply(
                matrix_rotation(angle),
                matrix_scale(0.8f, 0.8f)
            )
        );
        
        VectorPath letter_R;
        path_init(&letter_R, 64);
        define_letter_R(&letter_R, -50, -50, 1.0f);  /* Center the letter */
        transform_path(&letter_R, transform);
        render_path(&letter_R, (Color){0.8f, 0.5f, 1.0f, 1}, (Color){1, 1, 1, 0.5f}, 2, FILL_RULE_NONZERO);
        path_free(&letter_R);
        
        /* Bezier curve demonstration */
        draw_bezier_demo(550, 420);
        
        /* Info text */
        render_text("Bezier Curve", 550, 580, 0.5f, 
                   (Color){0.7f, 0.7f, 0.7f, 1}, (Color){0, 0, 0, 0}, 0);
        
        /* Decorative elements */
        draw_border_frame();
        
        /* Update display */
        update_display();
        
        /* Handle events */
        XEvent event;
        while (XPending(display)) {
            XNextEvent(display, &event);
            if (event.type == KeyPress) {
                running = 0;
            }
        }
        
        time += 0.016f;
        
        /* Simple frame delay */
        struct timespec ts = {0, 16000000};
        nanosleep(&ts, NULL);
    }
    
    close_display();
    return 0;
}
