#include "config.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>

#include "gunixtimestampingmessage.h"
#include "gnetworking.h"
#include "gioerror.h"

enum TIMESTAMPINGTYPE {
    TIMESTAMPINGTYPE_SYSTEM,
    TIMESTAMPINGTYPE_TRANSFORMED,
    TIMESTAMPINGTYPE_RAW,
    __TIMESTAMPINGTYPE_MAX__
};

char * TIMESTAMPINGTYPE_NAME[__TIMESTAMPINGTYPE_MAX__] = {"SYSTEM","TRANSFORMED","RAW"};

enum PROPERTIES {
    PROP_0 = 0,
    PROP_TYPE,
    PROP_TYPE_NAME,
    PROP_SECONDS,
    PROP_NANOSECONDS,
    PROP_PACKET_ID
};

struct _GUnixTimestampingMessagePrivate
{
  gint mask;
  gint timestamping_type;
  glong timestamping_sec;
  glong timestamping_nsec;
  guint packet_id;
};

struct _GUnixTimestampingDeserializationData
{
  gboolean packet_id_is_set;
  gboolean timestamping_is_set;

  gint timestamping_type;
  glong timestamping_sec;
  glong timestamping_nsec;
  guint packet_id;
};

G_DEFINE_TYPE_WITH_PRIVATE (GUnixTimestampingMessage, g_unix_timestamping_message, G_TYPE_SOCKET_CONTROL_MESSAGE)

static gsize g_unix_timestamping_message_get_size (GSocketControlMessage * message)
{
  printf ("** TUNK ** g_unix_timestamping_message_get_size\n");
  return G_UNIX_TIMESTAMPING_SEND_SIZE;
}

static int
g_unix_timestamping_message_get_level (GSocketControlMessage * message)
{
  printf ("** TUNK ** enter %s\n", __FUNCTION__);
  return SOL_SOCKET;
}

static int
g_unix_timestamping_message_get_msg_type (GSocketControlMessage * message)
{
    printf ("** TUNK ** enter %s\n", __FUNCTION__);
  return SCM_TIMESTAMPING;
}

static GPrivate timestampingDeserializationData = G_PRIVATE_INIT (g_free);

static GSocketControlMessage *
g_unix_timestamping_message_deserialize (int level, int type, gsize size, gpointer data)
{
  GUnixTimestampingDeserializationData * deserializationData = g_private_get(&timestampingDeserializationData);
  if (deserializationData == NULL){
    deserializationData = g_new0(GUnixTimestampingDeserializationData, 1);
    g_private_set(&timestampingDeserializationData, deserializationData);
  }


  printf ("** TUNK ** enter %s\n", __FUNCTION__);

  if ((level == SOL_IP || level == SOL_IPV6) && type == IP_RECVERR)
  {
    struct sock_extended_err * errqueue = (struct sock_extended_err *)data;
    if (errqueue->ee_errno == ENOMSG && errqueue->ee_origin == SO_EE_ORIGIN_TIMESTAMPING) {
      deserializationData->packet_id = errqueue->ee_data;
      deserializationData->packet_id_is_set = TRUE;      
      printf("** TUNK ** GOT IP_RECVERR! PacketID:%u\n", deserializationData->packet_id);
    }
  }

  if (level == SOL_SOCKET && type == SCM_TIMESTAMPING)
  {
    struct timespec* udp_tx_stamp = (struct timespec*) data;

    if (size != G_UNIX_TIMESTAMPING_RECV_SIZE || data == NULL)
    {
      g_warning ("Expected a timestamping struct of %" G_GSIZE_FORMAT " bytes but " "got %" G_GSIZE_FORMAT " bytes of data", G_UNIX_TIMESTAMPING_RECV_SIZE, size);
      return NULL;
    }

    if (udp_tx_stamp[TIMESTAMPINGTYPE_RAW].tv_sec != 0 || udp_tx_stamp[TIMESTAMPINGTYPE_RAW].tv_nsec != 0 ) {
      deserializationData->timestamping_type = TIMESTAMPINGTYPE_RAW;
    } else if (udp_tx_stamp[TIMESTAMPINGTYPE_SYSTEM].tv_sec != 0 || udp_tx_stamp[TIMESTAMPINGTYPE_SYSTEM].tv_nsec != 0 ){
      deserializationData->timestamping_type = TIMESTAMPINGTYPE_SYSTEM;
    } else if (udp_tx_stamp[TIMESTAMPINGTYPE_TRANSFORMED].tv_sec != 0 || udp_tx_stamp[TIMESTAMPINGTYPE_TRANSFORMED].tv_nsec != 0 ) {
      deserializationData->timestamping_type = TIMESTAMPINGTYPE_TRANSFORMED;
    } else {
      g_warning ("Received timestamping information with empty timestamping info, ignoring.");
      return NULL;
    }
    deserializationData->timestamping_sec = udp_tx_stamp[deserializationData->timestamping_type].tv_sec;
    deserializationData->timestamping_nsec = udp_tx_stamp[deserializationData->timestamping_type].tv_nsec;
    deserializationData->timestamping_is_set = TRUE;
      printf("** TUNK ** GOT SCM_TIMESTAMPING! Timer:%s timestamp:%ld.%09ld\n", TIMESTAMPINGTYPE_NAME[deserializationData->timestamping_type], deserializationData->timestamping_sec, deserializationData->timestamping_nsec);
  }

  if (deserializationData->packet_id_is_set && deserializationData->timestamping_is_set) {
    GSocketControlMessage * data = g_object_new (G_TYPE_UNIX_TIMESTAMPING_MESSAGE,
               "timestampingtype", deserializationData->timestamping_type,
               "timestampingtypename", TIMESTAMPINGTYPE_NAME[deserializationData->timestamping_type],
               "timestampingsecs", deserializationData->timestamping_sec,
               "timestampingnsecs", deserializationData->timestamping_nsec,
               "packetid", deserializationData->packet_id,
               NULL);
    g_private_replace(&timestampingDeserializationData, NULL);
    printf("** TUNK ** RETURNING assembled GSocketControlMessage!\n");
    return data;
  }
  return NULL;
}

static void
g_unix_timestamping_message_serialize (GSocketControlMessage * _message, gpointer _data)
{
  guint *data = (guint *) _data;
  printf ("** TUNK ** enter %s\n", __FUNCTION__);
  *data = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
}

static void
g_unix_timestamping_message_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  GUnixTimestampingMessage *message = G_UNIX_TIMESTAMPING_MESSAGE (object);
  printf ("** TUNK ** enter %s, object:%p prop_id:%d\n", __FUNCTION__, object, prop_id);

  switch (prop_id)
  {
    case PROP_TYPE:
        g_value_set_int (value, message->priv->timestamping_type);
        break;
    case PROP_TYPE_NAME:
        g_value_set_string(value,TIMESTAMPINGTYPE_NAME[message->priv->timestamping_type]);
        break;
    case PROP_SECONDS:
        g_value_set_int64 (value, message->priv->timestamping_sec);
        break;
    case PROP_NANOSECONDS:
        g_value_set_int64 (value, message->priv->timestamping_nsec);
        break;
    case PROP_PACKET_ID:
        g_value_set_uint (value, message->priv->packet_id);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
  }
  printf ("** TUNK ** leave %s\n", __FUNCTION__);
}


static void
g_unix_timestamping_message_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  GUnixTimestampingMessage *message = G_UNIX_TIMESTAMPING_MESSAGE (object);

  GObjectClass *object_class = G_OBJECT_GET_CLASS(object);
  GUnixTimestampingMessageClass *scm_class = G_UNIX_TIMESTAMPING_MESSAGE_CLASS (object_class);

  printf ("** TUNK ** enter %s, object:%p prop_id:%d\n", __FUNCTION__, object, prop_id);
  printf("CLASS: %s TYPE: %ld Expected: %d\n", G_OBJECT_CLASS_NAME(object_class), G_OBJECT_CLASS_TYPE(object_class), G_IS_UNIX_TIMESTAMPING_MESSAGE_CLASS(object_class));
  printf("VAL: %lu\n", scm_class->tunkptr++);

  switch (prop_id)
    {
    case PROP_TYPE:
      message->priv->timestamping_type = g_value_get_int(value);
      printf ("** TUNK ** prop_id:%d:PROP_TYPE Value:%d\n", prop_id, message->priv->timestamping_type);
      break;
    case PROP_TYPE_NAME:
      printf ("** TUNK ** prop_id:%d:PROP_TYPE_NAME Value:%s\n", prop_id, TIMESTAMPINGTYPE_NAME[message->priv->timestamping_type]);
      break;
    case PROP_SECONDS:
      message->priv->timestamping_sec = g_value_get_int64(value);
      printf ("** TUNK ** prop_id:%d:PROP_SECONDS Value:%ld\n", prop_id, message->priv->timestamping_sec);
      break;
    case PROP_NANOSECONDS:
      message->priv->timestamping_nsec = g_value_get_int64(value);
      printf ("** TUNK ** prop_id:%d:PROP_NANOSECONDS Value:%ld\n", prop_id, message->priv->timestamping_nsec);
      break;
    case PROP_PACKET_ID:
      message->priv->packet_id = g_value_get_uint(value);
      printf ("** TUNK ** prop_id:%d:PROP_PACKET_ID Value:%u\n", prop_id, message->priv->packet_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
  printf ("** TUNK ** leave %s\n", __FUNCTION__);
}

static void
g_unix_timestamping_message_init (GUnixTimestampingMessage * message)
{
  printf ("** TUNK ** enter %s\n", __FUNCTION__);
  message->priv = g_unix_timestamping_message_get_instance_private(message);
  printf ("** TUNK ** leave %s\n", __FUNCTION__);
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

  printf ("** TUNK ** enter %s\n", __FUNCTION__);

  class->tunkptr = 123;

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
                            TIMESTAMPINGTYPE_SYSTEM,
                            TIMESTAMPINGTYPE_RAW,
                            TIMESTAMPINGTYPE_SYSTEM,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_TYPE_NAME,
                   g_param_spec_string ("timestampingtypename",
                            "Timestamp_type_name",
                            "Timestamping typename for outgoing packet.",
                            "Unknown_type_name",
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
  g_object_class_install_property (object_class, PROP_PACKET_ID,
                   g_param_spec_uint ("packetid",
                            "Packet_id",
              "Packet id of responding sent packet (counter per socket).",
                            0,
                            G_MAXUINT32,
                            0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  printf ("** TUNK ** leave %s\n", __FUNCTION__);
}

gboolean
g_unix_timestamping_message_is_supported (void)
{
  printf ("** TUNK ** enter %s\n", __FUNCTION__);
  return TRUE;
}

GSocketControlMessage *
g_unix_timestamping_message_new (void)
{
  GUnixTimestampingMessage * msg = NULL;
  printf ("** TUNK ** enter %s\n", __FUNCTION__);
  msg =  g_object_new (G_TYPE_UNIX_TIMESTAMPING_MESSAGE, NULL);
  printf ("** TUNK ** leave %s\n", __FUNCTION__);
  return msg;
}

//void * g_unix_timestamping_