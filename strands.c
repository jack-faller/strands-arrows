#include "cairo.h"
#include "gdk/gdk.h"
#include "gio/gio.h"
#include "glib-object.h"
#include "glib.h"
#include <getopt.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>

#define eprintf(FORMAT, ...) fprintf(stderr, FORMAT, __VA_ARGS__)
#define LENGTH(ARRAY) (sizeof(ARRAY) / sizeof((ARRAY)[0]))

typedef struct CharCounts {
  GHashTable *table;
  size_t max, total;
} CharCounts;
typedef struct AppInfo {
  CharCounts counts;
  GtkAdjustment *slider;
  GtkTextBuffer *buffer;
  GtkDrawingArea *canvas;
} AppInfo;

#define UTF8_SIZE 6
#define N_UTF8_SIZE(N) (UTF8_SIZE * (N) + 1)
static gunichar next_in_line(GtkTextIter *iter);
static gunichar get_unichar(GInputStream *stream);
static void shift(size_t array_len, gunichar *array, gunichar next);
static void contract_utf8(char *dst, gunichar *src, int count);
static void expand_utf8(gunichar *dst, char *src, int count);
// Take ownership of file.
static void counts_add(CharCounts *counts, GFile *file);
static void counts_add_callback(gpointer key, gpointer value,
                                gpointer char_counts);
static void counts_clear(CharCounts *counts);
static gboolean counts_clear_callback(gpointer key, gpointer value,
                                      gpointer char_counts);
static void redraw(GtkWidget *ignored, gpointer app_info);

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
                 gpointer app_info);
static void rel_arrow_to(cairo_t *cr, double x, double y);
static void activate(GtkApplication *area, gpointer app_info);
static void on_clear(GtkWidget *button, gpointer app_info);
static void on_load(GtkWidget *button, gpointer app_info);
static void on_load_file_chosen(GObject *dialog, GAsyncResult *result,
                                gpointer app_info);

static struct option options[] = {{"word-file", true, NULL, 'w'}, {}};

int main(int argc, char **argv) {
  AppInfo info = {{g_hash_table_new(g_str_hash, g_str_equal)}};
  for (char c; -1 != (c = getopt_long(argc, argv, "w:", options, NULL));) {
    switch (c) {
    default:
      return 1;
    case 'w':
      counts_add(&info.counts, g_file_new_for_path(optarg));
      break;
    }
  }

  GtkApplication *app =
      gtk_application_new("org.gtk.example", G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option(G_APPLICATION(app), "word-file", 'w',
                                G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
                                "Read word frequencies from file",
                                "file to read from");
  g_signal_connect(app, "activate", G_CALLBACK(activate), &info);
  int status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return status;
}
void activate(GtkApplication *app, gpointer app_info) {
  AppInfo *info = app_info;
  GtkWindow *window = GTK_WINDOW(gtk_application_window_new(app));
  gtk_window_set_title(GTK_WINDOW(window), "Strands Analysis");

  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_window_set_child(window, GTK_WIDGET(box));

  GtkWidget *canvas = gtk_drawing_area_new();
  info->canvas = GTK_DRAWING_AREA(canvas);
  gtk_drawing_area_set_draw_func(info->canvas, draw, info, NULL);
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), canvas);
  gtk_widget_set_hexpand(scrolled, true);
  gtk_widget_set_vexpand(scrolled, true);
  gtk_box_append(box, scrolled);

  info->slider = gtk_adjustment_new(1, 0, 1, 1. / 1024, 1. / 1024, 1. / 1024);
  g_signal_connect(info->slider, "value-changed", G_CALLBACK(redraw), info);
  GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, info->slider);
  gtk_box_append(box, scale);

  info->buffer = gtk_text_buffer_new(NULL);
  g_signal_connect(info->buffer, "changed", G_CALLBACK(redraw), info);
  gtk_box_append(box, gtk_text_view_new_with_buffer(info->buffer));

  GtkBox *buttons = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  gtk_box_append(box, GTK_WIDGET(buttons));

  GtkWidget *load = gtk_button_new_with_label("Load");
  g_signal_connect(load, "clicked", G_CALLBACK(on_load), app_info);
  gtk_box_append(buttons, load);
  GtkWidget *clear = gtk_button_new_with_label("Clear");
  g_signal_connect(clear, "clicked", G_CALLBACK(on_clear), app_info);
  gtk_box_append(buttons, clear);

  gtk_window_set_default_size(GTK_WINDOW(window), 200, 200);
  gtk_window_present(GTK_WINDOW(window));
}

void rel_arrow_to(cairo_t *cr, double x, double y) {
  const float head_length = 7;
  double length = sqrt(x * x + y * y), xnorm = x / length, ynorm = y / length;
  cairo_rel_line_to(cr, x, y);
  cairo_get_current_point(cr, &x, &y);
  double scale = sqrt(0.5) * head_length;
  for (int sign = -1; sign <= 1; sign += 2) {
    cairo_move_to(cr, x, y);
    cairo_rel_line_to(cr, scale * -(xnorm + sign * ynorm),
                      scale * (sign * xnorm - ynorm));
  }
  cairo_stroke(cr);
}
void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
          gpointer app_info) {
  const int letter_gap = 50, arrow_gap = 3, font_size = 30;
  AppInfo *info = app_info;
  int line_count = gtk_text_buffer_get_line_count(info->buffer);

  GdkRGBA color;
  gtk_widget_get_color(GTK_WIDGET(area), &color);
  gdk_cairo_set_source_rgba(cr, &color);
  cairo_set_font_size(cr, font_size);
  // For arrows.
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  double frequency_max = (double)info->counts.max / info->counts.total;
  double frequency_threshold =
      frequency_max * (1 - gtk_adjustment_get_value(info->slider));

  for (int line = 0; line < line_count; ++line) {
    bool has_line[3] = {line != 0, true, line != line_count - 1};
    GtkTextIter lines[3];
    gunichar surrounding[LENGTH(lines)][3] = {};
    for (int i = 0; i < LENGTH(lines); ++i)
      if (has_line[i]) {
        gtk_text_buffer_get_iter_at_line(info->buffer, &lines[i], line + i - 1);
        surrounding[i][2] = next_in_line(&lines[i]);
      }
    for (int column = 0; surrounding[1][2] != 0; ++column) {
      for (int i = 0; i < LENGTH(lines); ++i)
        if (has_line[i])
          shift(LENGTH(surrounding[i]), surrounding[i],
                next_in_line(&lines[i]));
      char current_letter[N_UTF8_SIZE(1)];
      contract_utf8(current_letter, &surrounding[1][1], 1);
      cairo_text_extents_t extents;
      cairo_text_extents(cr, current_letter, &extents);
      int center_x = (column + 1) * letter_gap;
      int center_y = (line + 1) * letter_gap;
      cairo_move_to(cr, center_x - extents.width / 2,
                    center_y + font_size / 2.);
      cairo_show_text(cr, current_letter);
      for (int y1 = -1; y1 <= 1; ++y1)
        for (int x1 = -1; x1 <= 1; ++x1)
          if ((x1 != 0 || y1 != 0) && 0 != surrounding[y1 + 1][x1 + 1]) {
            double frequency = 0;
            for (int y0 = -1; y0 <= 1; ++y0)
              for (int x0 = -1; x0 <= 1; ++x0)
                if ((x0 != 0 || y0 != 0) && (x0 != x1 || y0 != y1) &&
                    0 != surrounding[y0 + 1][x0 + 1]) {
                  char buf[N_UTF8_SIZE(3)];
                  gunichar chars[3] = {surrounding[y0 + 1][x0 + 1],
                                       surrounding[1][1],
                                       surrounding[y1 + 1][x1 + 1]};
                  contract_utf8(buf, chars, LENGTH(chars));
                  frequency =
                      MAX(frequency,
                          (size_t)g_hash_table_lookup(info->counts.table, buf) /
                              (double)info->counts.total);
                }
            if (frequency > frequency_threshold) {
              cairo_move_to(cr, center_x + x1 * (font_size / 2. + arrow_gap),
                            center_y + y1 * (font_size / 2. + arrow_gap));
              rel_arrow_to(cr, (letter_gap - font_size - arrow_gap * 2) * x1,
                           (letter_gap - font_size - arrow_gap * 2) * y1);
            }
          }
    }
  }
}
void redraw(GtkWidget *ignored, gpointer app_info) {
  AppInfo *info = app_info;
  gtk_widget_queue_draw(GTK_WIDGET(info->canvas));
}

void on_clear(GtkWidget *button, gpointer app_info) {
  AppInfo *info = app_info;
  counts_clear(&info->counts);
  redraw(NULL, info);
}
void on_load(GtkWidget *button, gpointer app_info) {
  GtkFileDialog *d = gtk_file_dialog_new();
  gtk_file_dialog_open(d, GTK_WINDOW(gtk_widget_get_root(button)), NULL,
                       on_load_file_chosen, app_info);
  g_object_unref(d);
}
void on_load_file_chosen(GObject *dialog, GAsyncResult *result,
                         gpointer app_info) {
  AppInfo *info = app_info;
  GFile *file =
      gtk_file_dialog_open_finish(GTK_FILE_DIALOG(dialog), result, NULL);
  if (file == NULL)
    return;
  counts_add(&info->counts, file);
  redraw(NULL, info);
}

gunichar next_in_line(GtkTextIter *iter) {
start:
  if (gtk_text_iter_is_end(iter))
    return 0;
  gunichar c;
  switch ((c = gtk_text_iter_get_char(iter))) {
  case '\r':
  case '\n':
    return 0;
  default:
    gtk_text_iter_forward_char(iter);
    if (g_unichar_isalpha(c))
      return g_unichar_toupper(c);
    else
      goto start;
  }
}
gunichar get_unichar(GInputStream *stream) {
  char buffer[UTF8_SIZE];
  gunichar out;
  for (int count = 0;; ++count) {
    if (0 == g_input_stream_read(stream, &buffer[count], 1, NULL, NULL))
      return 0;
    if (-2 != (out = g_utf8_get_char_validated(buffer, count)))
      return g_unichar_toupper(out);
  }
}
void contract_utf8(char *dst, gunichar *src, int count) {
  for (int i = 0; i < count; ++i)
    dst += g_unichar_to_utf8(src[i], dst);
  *dst = 0;
}
void expand_utf8(gunichar *dst, char *src, int count) {
  for (int i = 0; i < count; ++i) {
    dst[i] = g_utf8_get_char_validated(src, -1);
    src = g_utf8_next_char(src);
  }
}
void shift(size_t array_len, gunichar *array, gunichar next) {
  for (int i = 1; i < array_len; ++i)
    array[i - 1] = array[i];
  array[array_len - 1] = next;
}
void counts_add_callback(gpointer key, gpointer value, gpointer char_counts) {
  CharCounts *counts = char_counts;
  counts->max = MAX(counts->max, (size_t)value);
  counts->total += (size_t)value;
}
void counts_add(CharCounts *counts, GFile *file) {
  GError *error;
  GInputStream *base = G_INPUT_STREAM(g_file_read(file, NULL, &error));
  if (base == NULL) {
    eprintf("%s\n", error->message);
    g_error_free(error);
    return;
  }
  GInputStream *input = g_buffered_input_stream_new(base);

  gunichar read_long[3];
  read_long[0] = 0;
  for (int i = 1; i < LENGTH(read_long); ++i)
    if (0 == (read_long[i] = get_unichar(input)))
      return;
  gunichar c;
  while (0 != (c = get_unichar(input))) {
    shift(LENGTH(read_long), read_long, g_unichar_toupper(c));
    for (int i = 0; i < LENGTH(read_long); ++i)
      if (!g_unichar_isalpha(read_long[i]))
        goto end;
    char read_variable[N_UTF8_SIZE(3)], *read_ptr = read_variable;
    contract_utf8(read_variable, read_long, LENGTH(read_long));
    gpointer count_ptr = NULL;
    if (!g_hash_table_lookup_extended(counts->table, read_ptr, NULL,
                                      &count_ptr))
      read_ptr = strdup(read_ptr);
    g_hash_table_insert(counts->table, read_ptr,
                        (gpointer)((size_t)count_ptr + 1));
  end:;
  }
  counts->total = 0;
  counts->max = 0;
  g_hash_table_foreach(counts->table, counts_add_callback, counts);

  g_input_stream_close(input, NULL, NULL);
  g_input_stream_close(base, NULL, NULL);
  g_object_unref(base);
  g_object_unref(input);
  g_object_unref(file);
}
gboolean counts_clear_callback(gpointer key, gpointer value,
                               gpointer char_counts) {
  free(key);
  return true;
}
void counts_clear(CharCounts *counts) {
  counts->total = 0;
  counts->max = 0;
  g_hash_table_foreach_remove(counts->table, counts_clear_callback, counts);
}
