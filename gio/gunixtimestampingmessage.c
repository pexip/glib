#include "config.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "gsockettimestampingmessage.h"
#include "gnetworking.h"
#include "gioerror.h"

struct _GSocketTimestampingMessagePrivate
{
  int mask;
};

G_DEFINE_TYPE_WITH_PRIVATE (GSocketTimestampingMessage, g_socket_timestamping_message, G_TYPE_SOCKET_CONTROL_MESSAGE)

static gsize
g_socket_timestamping_message_get_size (GSocketControlMessage *message)
{
  return G_SOCKET_TIMESTAMPING_NATIVE_SIZE;
}

static int
g_socket_timestamping_message_get_level (GSocketControlMessage *message)
{
  return SOL_SOCKET;
}

static int
g_socket_timestamping_message_get_msg_type (GSocketControlMessage *message)
{
  return SCM_TIMESTAMPING;
}

static GSocketControlMessage *
g_socket_timestamping_message_deserialize (int level, int type, gsize size, gpointer data)
{
    if (level != SOL_SOCKET || type != SCM_TIMESTAMPING){
        return NULL;
    }
    if (size != G_SOCKET_TIMESTAMPING_NATIVE_SIZE || data == NULL){
        g_warning ("Expected a timestamping struct of %" G_GSIZE_FORMAT " bytes but "
                   "got %" G_GSIZE_FORMAT " bytes of data",
                   G_SOCKET_TIMESTAMPING_NATIVE_SIZE, size);

        return NULL;
    }

    return g_object_new (G_TYPE_SOCKET_TIMESTAMPING_MESSAGE,
                         "timestamping", data,
                         NULL);
}

static void
g_socket_timestamping_message_serialize (GSocketControlMessage *_message, gpointer data)
{
    GSocketTimestampingMessage *message = G_SOCKET_TIMESTAMPING_MESSAGE (_message);

    memcpy(data, &message->priv->mask, G_SOCKET_TIMESTAMPING_NATIVE_SIZE);
}

static void
g_socket_timestamping_message_init (GSocketTimestampingMessage *message)
{
  message->priv = NULL;
}

/*
static void
g_socket_timestamping_message_finalize (GObject *object)
{
  GSocketTimestampingMessage *message = G_SOCKET_TIMESTAMPING_MESSAGE (object);

  G_OBJECT_CLASS (g_socket_timestamping_message_parent_class)
    ->finalize (object);
}
*/

static void
g_socket_timestamping_message_class_init (GSocketTimestampingMessageClass *class)
{
  GSocketControlMessageClass *scm_class = G_SOCKET_CONTROL_MESSAGE_CLASS (class);
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  scm_class->get_size = g_socket_timestamping_message_get_size;
  scm_class->get_level = g_socket_timestamping_message_get_level;
  scm_class->get_type = g_socket_timestamping_message_get_msg_type;
  scm_class->serialize = g_socket_timestamping_message_serialize;
  scm_class->deserialize = g_socket_timestamping_message_deserialize;
  object_class->finalize = NULL;
  object_class->set_property = NULL;
  object_class->get_property = NULL;

}

gboolean
g_socket_timestamping_message_is_supported (void)
{
    return TRUE;
}