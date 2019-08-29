#include "config.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/net_tstamp.h>

#include "gunixtimestampingmessage.h"
#include "gnetworking.h"
#include "gioerror.h"

enum TIMESTAMPINGTYPE {
    TIMESTAMP_SYSTEM,
    TIMESTAMP_TRANSFORMED,
    TIMESTAMP_RAW
};

enum PROPERTIES {
    PROP_0 = 0,
    PROP_TYPE,
    PROP_SECONDS,
    PROP_NANOSECONDS
};

struct _GUnixTimestampingMessagePrivate
{
  gint mask;
  gint timestampingtype;
  glong timestampingsec;
  glong timestampingnsec;
};

G_DEFINE_TYPE_WITH_PRIVATE (GUnixTimestampingMessage,
			    g_unix_timestamping_message,
			    G_TYPE_SOCKET_CONTROL_MESSAGE)
     static gsize g_unix_timestamping_message_get_size (GSocketControlMessage
							* message)
{
  printf ("** TUNK ** g_unix_timestamping_message_get_size\n");
  return G_UNIX_TIMESTAMPING_SEND_SIZE;
}

static int
g_unix_timestamping_message_get_level (GSocketControlMessage * message)
{
  printf ("** TUNK ** g_unix_timestamping_message_get_level\n");
  return SOL_SOCKET;
}

static int
g_unix_timestamping_message_get_msg_type (GSocketControlMessage * message)
{
  printf ("** TUNK ** g_unix_timestamping_message_get_msg_type\n");
  return SCM_TIMESTAMPING;
}

static GSocketControlMessage *
g_unix_timestamping_message_deserialize (int level, int type, gsize size,
					 gpointer data)
{
  struct timespec* udp_tx_stamp = NULL;
  gint timestampingtype = -1;
  glong timestampingsec = -1;
  glong timestampingnsec = -1;

  printf ("** TUNK ** g_unix_timestamping_message_deserialize\n");

  if (level != SOL_SOCKET || type != SCM_TIMESTAMPING)
    {
      return NULL;
    }
  if (size != G_UNIX_TIMESTAMPING_RECV_SIZE || data == NULL)
    {
      g_warning ("Expected a timestamping struct of %" G_GSIZE_FORMAT
		 " bytes but " "got %" G_GSIZE_FORMAT " bytes of data",
		 G_UNIX_TIMESTAMPING_RECV_SIZE, size);

      return NULL;
    }

  udp_tx_stamp = (struct timespec*) data;
  if (udp_tx_stamp[TIMESTAMP_RAW].tv_sec != 0 || udp_tx_stamp[TIMESTAMP_RAW].tv_nsec != 0 ) {
    timestampingtype = TIMESTAMP_RAW;
  } else if (udp_tx_stamp[TIMESTAMP_SYSTEM].tv_sec != 0 || udp_tx_stamp[TIMESTAMP_SYSTEM].tv_nsec != 0 ){
      timestampingtype = TIMESTAMP_SYSTEM;
  } else if (udp_tx_stamp[TIMESTAMP_TRANSFORMED].tv_sec != 0 || udp_tx_stamp[TIMESTAMP_TRANSFORMED].tv_nsec != 0 ) {
      timestampingtype = TIMESTAMP_TRANSFORMED;
}

  return g_object_new (G_TYPE_UNIX_TIMESTAMPING_MESSAGE,
               "timestampingtype", timestampingtype,
               "timestampingsecs", udp_tx_stamp[timestampingtype].tv_sec,
               "timestampingnsecs", udp_tx_stamp[timestampingtype].tv_nsec,
               NULL);
}

static void
g_unix_timestamping_message_serialize (GSocketControlMessage * _message,
				       gpointer _data)
{
  guint *data = (guint *) _data;
  printf ("** TUNK ** g_unix_timestamping_message_serialize\n");
  *data = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
}

static void
g_unix_timestamping_message_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  GUnixTimestampingMessage *message = G_UNIX_TIMESTAMPING_MESSAGE (object);
  printf ("** TUNK ** g_unix_timestamping_message_get_property\n");
  printf("** TUNK ** PROP_ID: %d\n", prop_id);

  switch (prop_id)
  {
    case PROP_TYPE:
        g_value_set_int (value, message->priv->timestampingtype);
        break;
    case PROP_SECONDS:
        g_value_set_int64 (value, message->priv->timestampingsec);
        break;
    case PROP_NANOSECONDS:
        g_value_set_int64 (value, message->priv->timestampingnsec);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
  }
  printf ("** TUNK ** OUT g_unix_timestamping_message_get_property\n");
}


static void
g_unix_timestamping_message_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  GUnixTimestampingMessage *message = G_UNIX_TIMESTAMPING_MESSAGE (object);
  printf ("** TUNK ** g_unix_timestamping_message_set_property\n");

  printf("** TUNK ** PROP_ID: %d\n", prop_id);
  switch (prop_id)
    {
    case PROP_TYPE:
      message->priv->timestampingtype = g_value_get_int(value);
      break;
    case PROP_SECONDS:
      message->priv->timestampingsec = g_value_get_int64(value);
      break;
    case PROP_NANOSECONDS:
      message->priv->timestampingnsec = g_value_get_int64(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
  printf ("** TUNK ** OUT g_unix_timestamping_message_set_property\n");
}



static void
g_unix_timestamping_message_init (GUnixTimestampingMessage * message)
{
  printf ("** TUNK ** g_unix_timestamping_message_init\n");
  message->priv = g_unix_timestamping_message_get_instance_private(message);
}

/*
static void
g_unix_timestamping_message_finalize (GObject *object)
{
  GUnixTimestampingMessage *message = G_UNIX_TIMESTAMPING_MESSAGE (object);

  G_OBJECT_CLASS (g_unix_timestamping_message_parent_class)
    ->finalize (object);
}
*/

static void
g_unix_timestamping_message_class_init (GUnixTimestampingMessageClass * class)
{
  GSocketControlMessageClass *scm_class = G_SOCKET_CONTROL_MESSAGE_CLASS (class);
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  printf ("** TUNK ** g_unix_timestamping_message_class_init\n");
  scm_class->get_size = g_unix_timestamping_message_get_size;
  scm_class->get_level = g_unix_timestamping_message_get_level;
  scm_class->get_type = g_unix_timestamping_message_get_msg_type;
  scm_class->serialize = g_unix_timestamping_message_serialize;
  scm_class->deserialize = g_unix_timestamping_message_deserialize;
  object_class->finalize = NULL;
  object_class->set_property = g_unix_timestamping_message_set_property;
  object_class->get_property = g_unix_timestamping_message_get_property;

  g_object_class_install_property (object_class, PROP_TYPE,
                   g_param_spec_int ("timestampingtype",
							"Timestamp_type",
							"Timestamping type for outgoing packet.",
                            TIMESTAMP_SYSTEM,
                            TIMESTAMP_RAW,
                            TIMESTAMP_SYSTEM,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_SECONDS,
                   g_param_spec_int64 ("timestampingsecs",
							"Timestamp_secs",
							"Timestamping seconds of outgoing packet.",
                            0,
                            G_MAXINT64,
                            0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_NANOSECONDS,
                   g_param_spec_int64 ("timestampingnsecs",
                            "Timestamp_nsecs",
							"Timestamping nanoseconds of outgoing packet.",
                            0,
                            G_MAXINT64,
                            0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  printf ("** TUNK ** OUT g_unix_timestamping_message_class_init\n");
}

gboolean
g_unix_timestamping_message_is_supported (void)
{
  printf ("** TUNK ** g_unix_timestamping_message_is_supported\n");
  return TRUE;
}
