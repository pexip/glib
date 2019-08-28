#include "config.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/net_tstamp.h>

#include "gunixtimestampingmessage.h"
#include "gnetworking.h"
#include "gioerror.h"

struct _GUnixTimestampingMessagePrivate
{
  int mask;
};

G_DEFINE_TYPE_WITH_PRIVATE (GUnixTimestampingMessage, g_unix_timestamping_message, G_TYPE_SOCKET_CONTROL_MESSAGE)

static gsize
g_unix_timestamping_message_get_size (GSocketControlMessage *message)
{
    printf("** TUNK ** g_unix_timestamping_message_get_size\n");
  return G_UNIX_TIMESTAMPING_SEND_SIZE;
}

static int
g_unix_timestamping_message_get_level (GSocketControlMessage *message)
{
    printf("** TUNK ** g_unix_timestamping_message_get_level\n");
  return SOL_SOCKET;
}

static int
g_unix_timestamping_message_get_msg_type (GSocketControlMessage *message)
{
    printf("** TUNK ** g_unix_timestamping_message_get_msg_type\n");
  return SCM_TIMESTAMPING;
}

static GSocketControlMessage *
g_unix_timestamping_message_deserialize (int level, int type, gsize size, gpointer data)
{
    printf("** TUNK ** g_unix_timestamping_message_deserialize\n");

    if (level != SOL_SOCKET || type != SCM_TIMESTAMPING){
        return NULL;
    }
    if (size != G_UNIX_TIMESTAMPING_RECV_SIZE || data == NULL){
        g_warning ("Expected a timestamping struct of %" G_GSIZE_FORMAT " bytes but "
                   "got %" G_GSIZE_FORMAT " bytes of data",
                   G_UNIX_TIMESTAMPING_RECV_SIZE, size);

        return NULL;
    }

    return g_object_new (G_TYPE_UNIX_TIMESTAMPING_MESSAGE,
                         "timestamping_system", data,
                         "timestamping_transformed", data,
                         "timestamping_raw", data,
                         NULL);
}

static void
g_unix_timestamping_message_serialize (GSocketControlMessage *_message, gpointer _data)
{
    guint * data = (guint *) _data;
    printf("** TUNK ** g_unix_timestamping_message_serialize\n");
    *data = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
}

static void
g_unix_timestamping_message_init (GUnixTimestampingMessage *message)
{
    printf("** TUNK ** g_unix_timestamping_message_init\n");
    message->priv = NULL;
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
g_unix_timestamping_message_class_init (GUnixTimestampingMessageClass *class)
{
  GSocketControlMessageClass *scm_class = G_SOCKET_CONTROL_MESSAGE_CLASS (class);
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  printf("** TUNK ** g_unix_timestamping_message_class_init\n");
  scm_class->get_size = g_unix_timestamping_message_get_size;
  scm_class->get_level = g_unix_timestamping_message_get_level;
  scm_class->get_type = g_unix_timestamping_message_get_msg_type;
  scm_class->serialize = g_unix_timestamping_message_serialize;
  scm_class->deserialize = g_unix_timestamping_message_deserialize;
  object_class->finalize = NULL;
  object_class->set_property = NULL;
  object_class->get_property = NULL;

  g_object_class_install_property (object_class, 1,
    g_param_spec_object ("timestamping_type", "Timestamp_type",
                         "Timestamping type for outgoing packet.",
                         G_TYPE_INT, G_PARAM_STATIC_STRINGS |
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

  g_object_class_install_property (object_class, 2,
    g_param_spec_object ("timestamping_secs", "Timestamp_secs",
                         "Timestamping seconds of outgoing packet.",
                         G_TYPE_INT64, G_PARAM_STATIC_STRINGS |
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, 3,
    g_param_spec_object ("timestamping_usecs", "Timestamp",
                         "Timestamping nanoseconds of outgoing packet.",
                         G_TYPE_INT64, G_PARAM_STATIC_STRINGS |
                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
  /*
  g_object_class_install_property (gobject_class,
                                   PROP_CREDENTIALS,
                                   g_param_spec_object ("credentials",
                                                        P_("Credentials"),
                                                        P_("The credentials stored in the message"),
                                                        G_TYPE_CREDENTIALS,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_NAME |
                                                        G_PARAM_STATIC_BLURB |
                                                        G_PARAM_STATIC_NICK));
                                                        */
}

gboolean
g_unix_timestamping_message_is_supported (void)
{
    printf("** TUNK ** g_unix_timestamping_message_is_supported\n");
    return TRUE;
}
