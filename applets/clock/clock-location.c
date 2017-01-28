#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "clock-location.h"
#include "clock-marshallers.h"
#include "set-timezone.h"
#include "system-timezone.h"

G_DEFINE_TYPE (ClockLocation, clock_location, G_TYPE_OBJECT)

typedef struct {
        gchar *name;
        gchar *city;

        SystemTimezone *systz;

        gchar *timezone;

        gchar *tzname;

        gfloat latitude;
        gfloat longitude;

        gchar *weather_code;
        guint weather_timeout;
        guint weather_retry_time;
} ClockLocationPrivate;

#define WEATHER_TIMEOUT_BASE 30
#define WEATHER_TIMEOUT_MAX  1800
#define WEATHER_EMPTY_CODE   "-"

enum {
        WEATHER_UPDATED,
        SET_CURRENT,
        LAST_SIGNAL
};

static guint location_signals[LAST_SIGNAL] = { 0 };

static void clock_location_finalize (GObject *);
static void clock_location_set_tz (ClockLocation *this);
static void clock_location_unset_tz (ClockLocation *this);
static gboolean update_weather_info (gpointer data);
static void setup_weather_updates (ClockLocation *loc);

static gchar *clock_location_get_valid_weather_code (const gchar *code);

#define PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CLOCK_LOCATION_TYPE, ClockLocationPrivate))

ClockLocation *
clock_location_find_and_ref (GList       *locations,
                             const gchar *name,
                             const gchar *city,
                             const gchar *timezone,
                             gfloat       latitude,
                             gfloat       longitude,
                             const gchar *code)
{
        GList *l;
        ClockLocationPrivate *priv;

        for (l = locations; l != NULL; l = l->next) {
                priv = PRIVATE (l->data);

                if (priv->latitude == latitude &&
                    priv->longitude == longitude &&
                    g_strcmp0 (priv->weather_code, code) == 0 &&
                    g_strcmp0 (priv->timezone, timezone) == 0 &&
                    g_strcmp0 (priv->city, city) == 0 &&
                    g_strcmp0 (priv->name, name) == 0)
                        break;
        }

        if (l != NULL)
                return g_object_ref (CLOCK_LOCATION (l->data));
        else
                return NULL;
}

static ClockLocation *current_location = NULL;

static void
clock_location_class_init (ClockLocationClass *this_class)
{
        GObjectClass *g_obj_class = G_OBJECT_CLASS (this_class);

        g_obj_class->finalize = clock_location_finalize;

        location_signals[SET_CURRENT] =
                g_signal_new ("set-current",
                              G_OBJECT_CLASS_TYPE (g_obj_class),
                              G_SIGNAL_RUN_FIRST,
                              G_STRUCT_OFFSET (ClockLocationClass, set_current),
                              NULL, NULL,
                              g_cclosure_marshal_VOID__VOID,
                              G_TYPE_NONE, 0);

        g_type_class_add_private (this_class, sizeof (ClockLocationPrivate));
}

static void
network_changed (GNetworkMonitor *monitor,
                 gboolean         available,
                 ClockLocation   *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

}

static void
clock_location_init (ClockLocation *this)
{
        ClockLocationPrivate *priv = PRIVATE (this);
        GNetworkMonitor *monitor;

        priv->name = NULL;
        priv->city = NULL;

        priv->systz = system_timezone_new ();

        priv->timezone = NULL;

        priv->tzname = NULL;

        priv->latitude = 0;
        priv->longitude = 0;

        monitor = g_network_monitor_get_default();
        g_signal_connect (monitor, "network-changed",
                          G_CALLBACK (network_changed), this);
}

static void
clock_location_finalize (GObject *g_obj)
{
        ClockLocationPrivate *priv = PRIVATE (g_obj);
        GNetworkMonitor      *monitor;

        monitor = g_network_monitor_get_default ();
        g_signal_handlers_disconnect_by_func (monitor,
                                              G_CALLBACK (network_changed),
                                              CLOCK_LOCATION (g_obj));

        if (priv->name) {
                g_free (priv->name);
                priv->name = NULL;
        }

        if (priv->city) {
                g_free (priv->city);
                priv->city = NULL;
        }

        if (priv->systz) {
                g_object_unref (priv->systz);
                priv->systz = NULL;
        }

        if (priv->timezone) {
                g_free (priv->timezone);
                priv->timezone = NULL;
        }

        if (priv->tzname) {
                g_free (priv->tzname);
                priv->tzname = NULL;
        }

        if (priv->weather_code) {
                g_free (priv->weather_code);
                priv->weather_code = NULL;
        }

        if (priv->weather_timeout) {
                g_source_remove (priv->weather_timeout);
                priv->weather_timeout = 0;
        }

        G_OBJECT_CLASS (clock_location_parent_class)->finalize (g_obj);
}

const gchar *
clock_location_get_display_name (ClockLocation *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        if (priv->name && priv->name[0])
                return priv->name;
        else
                return priv->city;
}

const gchar *
clock_location_get_name (ClockLocation *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        return priv->name;
}

void
clock_location_set_name (ClockLocation *loc, const gchar *name)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        if (priv->name) {
                g_free (priv->name);
                priv->name = NULL;
        }

        priv->name = g_strdup (name);
}

const gchar *
clock_location_get_city (ClockLocation *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        return priv->city;
}

void
clock_location_set_city (ClockLocation *loc, const gchar *city)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        if (priv->city) {
                g_free (priv->city);
                priv->city = NULL;
        }

        priv->city = g_strdup (city);
}

gchar *
clock_location_get_timezone (ClockLocation *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        return priv->timezone;
}

void
clock_location_set_timezone (ClockLocation *loc, const gchar *timezone)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        if (priv->timezone) {
                g_free (priv->timezone);
                priv->timezone = NULL;
        }

        priv->timezone = g_strdup (timezone);
}

gchar *
clock_location_get_tzname (ClockLocation *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        return priv->tzname;
}

void
clock_location_get_coords (ClockLocation *loc, gfloat *latitude,
                               gfloat *longitude)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        *latitude = priv->latitude;
        *longitude = priv->longitude;
}

void
clock_location_set_coords (ClockLocation *loc, gfloat latitude,
                               gfloat longitude)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        priv->latitude = latitude;
        priv->longitude = longitude;
}

static void
clock_location_set_tzname (ClockLocation *this, const char *tzname)
{
        ClockLocationPrivate *priv = PRIVATE (this);

        if (priv->tzname) {
                if (strcmp (priv->tzname, tzname) == 0) {
                        return;
                }

                g_free (priv->tzname);
                priv->tzname = NULL;
        }

        if (tzname) {
                priv->tzname = g_strdup (tzname);
        } else {
                priv->tzname = NULL;
        }
}

static void
clock_location_set_tz (ClockLocation *this)
{
        ClockLocationPrivate *priv = PRIVATE (this);

        time_t now_t;
        struct tm now;

        if (priv->timezone == NULL) {
                return;
        }

        setenv ("TZ", priv->timezone, 1);
        tzset();

        now_t = time (NULL);
        localtime_r (&now_t, &now);

        if (now.tm_isdst > 0) {
                clock_location_set_tzname (this, tzname[1]);
        } else {
                clock_location_set_tzname (this, tzname[0]);
        }
}

static void
clock_location_unset_tz (ClockLocation *this)
{
        ClockLocationPrivate *priv = PRIVATE (this);
        const char *env_timezone;

        if (priv->timezone == NULL) {
                return;
        }

        env_timezone = system_timezone_get_env (priv->systz);

        if (env_timezone) {
                setenv ("TZ", env_timezone, 1);
        } else {
                unsetenv ("TZ");
        }
        tzset();
}

void
clock_location_localtime (ClockLocation *loc, struct tm *tm)
{
        time_t now;

        clock_location_set_tz (loc);

        time (&now);
        localtime_r (&now, tm);

        clock_location_unset_tz (loc);
}

gboolean
clock_location_is_current_timezone (ClockLocation *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);
        const char *zone;

        zone = system_timezone_get (priv->systz);

        if (zone)
                return strcmp (zone, priv->timezone) == 0;
        else
                return clock_location_get_offset (loc) == 0;
}

gboolean
clock_location_is_current (ClockLocation *loc)
{
        if (current_location == loc)
                return TRUE;
        else if (current_location != NULL)
                return FALSE;

        if (clock_location_is_current_timezone (loc)) {
                /* Note that some code in clock.c depends on the fact that
                 * calling this function can set the current location if
                 * there's none */
                current_location = loc;
                g_object_add_weak_pointer (G_OBJECT (current_location),
                                           (gpointer *)&current_location);
                g_signal_emit (current_location, location_signals[SET_CURRENT],
                               0, NULL);

                return TRUE;
        }

        return FALSE;
}


glong
clock_location_get_offset (ClockLocation *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);
        glong sys_timezone, local_timezone;
        glong offset;
        time_t t;
        struct tm *tm;

        t = time (NULL);

        unsetenv ("TZ");
        tm = localtime (&t);
        sys_timezone = timezone;

        if (tm->tm_isdst > 0) {
                sys_timezone -= 3600;
        }

        setenv ("TZ", priv->timezone, 1);
        tm = localtime (&t);
        local_timezone = timezone;

        if (tm->tm_isdst > 0) {
                local_timezone -= 3600;
        }

        offset = local_timezone - sys_timezone;

        clock_location_unset_tz (loc);

        return offset;
}

typedef struct {
        ClockLocation *location;
        GFunc callback;
        gpointer data;
        GDestroyNotify destroy;
} MakeCurrentData;

static void
make_current_cb (gpointer data, GError *error)
{
        MakeCurrentData *mcdata = data;

        if (error == NULL) {
                if (current_location)
                        g_object_remove_weak_pointer (G_OBJECT (current_location),
                                                      (gpointer *)&current_location);
                current_location = mcdata->location;
                g_object_add_weak_pointer (G_OBJECT (current_location),
                                           (gpointer *)&current_location);
                g_signal_emit (current_location, location_signals[SET_CURRENT],
                               0, NULL);
        }

        if (mcdata->callback)
                mcdata->callback (mcdata->data, error);
        else
                g_error_free (error);
}

static void
free_make_current_data (gpointer data)
{
        MakeCurrentData *mcdata = data;

        if (mcdata->destroy)
                mcdata->destroy (mcdata->data);

        g_object_unref (mcdata->location);
        g_free (mcdata);
}

void
clock_location_make_current (ClockLocation *loc,
                             GFunc          callback,
                             gpointer       data,
                             GDestroyNotify destroy)
{
        ClockLocationPrivate *priv = PRIVATE (loc);
        gchar *filename;
        MakeCurrentData *mcdata;

        if (loc == current_location) {
                if (destroy)
                        destroy (data);
                return;
        }

        if (clock_location_is_current_timezone (loc)) {
                if (current_location)
                        g_object_remove_weak_pointer (G_OBJECT (current_location),
                                                      (gpointer *)&current_location);
                current_location = loc;
                g_object_add_weak_pointer (G_OBJECT (current_location),
                                           (gpointer *)&current_location);
                g_signal_emit (current_location, location_signals[SET_CURRENT],
                               0, NULL);
                if (callback)
                               callback (data, NULL);
                if (destroy)
                        destroy (data);
                return;
        }

        mcdata = g_new (MakeCurrentData, 1);

        mcdata->location = g_object_ref (loc);
        mcdata->callback = callback;
        mcdata->data = data;
        mcdata->destroy = destroy;

        filename = g_build_filename (SYSTEM_ZONEINFODIR, priv->timezone, NULL);
        set_system_timezone_async (filename,
                                   (GFunc)make_current_cb,
                                   mcdata,
                                   free_make_current_data);
        g_free (filename);
}

static gchar *
clock_location_get_valid_weather_code (const gchar *code)
{
        if (!code || code[0] == '\0')
                return g_strdup (WEATHER_EMPTY_CODE);
        else
                return g_strdup (code);
}

const gchar *
clock_location_get_weather_code (ClockLocation *loc)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        return priv->weather_code;
}

void
clock_location_set_weather_code (ClockLocation *loc, const gchar *code)
{
        ClockLocationPrivate *priv = PRIVATE (loc);

        g_free (priv->weather_code);
        priv->weather_code = clock_location_get_valid_weather_code (code);

        setup_weather_updates (loc);
}

static void
set_weather_update_timeout (ClockLocation *loc)
{
}

static gboolean
update_weather_info (gpointer data)
{
        return TRUE;
}

static gchar *
rad2dms (gfloat lat, gfloat lon)
{
        gchar h, h2;
        gfloat d, deg, min, d2, deg2, min2;

        h = lat > 0 ? 'N' : 'S';
        d = fabs (lat);
        deg = floor (d);
        min = floor (60 * (d - deg));
        h2 = lon > 0 ? 'E' : 'W';
        d2 = fabs (lon);
        deg2 = floor (d2);
        min2 = floor (60 * (d2 - deg2));
        return g_strdup_printf ("%02d-%02d%c %02d-%02d%c",
                                (int)deg, (int)min, h,
                                (int)deg2, (int)min2, h2);
}

static void
setup_weather_updates (ClockLocation *loc)
{
}
