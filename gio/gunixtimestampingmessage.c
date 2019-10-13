#include "config.h"

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>


#include "gunixtimestampingmessage.h"
#include "gnetworking.h"
#include "gioerror.h"

enum PROPERTIES {
    PROP_0 = 0,
    PROP_MESSAGE_TYPE,
    PROP_PACKET_ID,
    PROP_TIMESTAMPING_TYPE,
    PROP_SECONDS,
    PROP_NANOSECONDS,
};

enum MESSAGE_TYPE {
  MESSAGE_TYPE_UNSET,
  MESSAGE_TYPE_PACKET_ID,
  MESSAGE_TYPE_TIMESTAMPING_INFO,
  __MESSAGE_TYPE_MAX__
};

char * MESSAGE_TYPE_TO_STRING[__MESSAGE_TYPE_MAX__] = {"UNSET", "PACKET_ID", "TIMESTAMPING_INFO"};

enum TIMESTAMPING_TYPE {
    TIMESTAMPING_TYPE_SYSTEM,
    TIMESTAMPING_TYPE_TRANSFORMED,
    TIMESTAMPING_TYPE_RAW,
    __TIMESTAMPING_TYPE_MAX__
};
char * TIMESTAMPING_TYPE_TO_STRING[__TIMESTAMPING_TYPE_MAX__] = {"SYSTEM","TRANSFORMED","RAW"};

struct _GUnixTimestampingMessagePrivate
{
  gint mask;

  guint message_type;
  guint timestamping_type;
  guint packet_id;
  glong timestamping_sec;
  glong timestamping_nsec;
};

struct _GUnixTimestampingMessageParsed
{
  guint packet_id;
  guint timestamping_type;
  glong timestamping_sec;
  glong timestamping_nsec;  
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

static GSocketControlMessage *
g_unix_timestamping_message_deserialize (int level, int type, gsize size, gpointer data)
{
  printf ("** TUNK ** enter %s\n", __FUNCTION__);

  if ((level == SOL_IP || level == SOL_IPV6) && type == IP_RECVERR)
  {
    struct sock_extended_err * errqueue = (struct sock_extended_err *)data;
    if (errqueue->ee_errno == ENOMSG && errqueue->ee_origin == SO_EE_ORIGIN_TIMESTAMPING) {
      GSocketControlMessage * data = g_object_new (G_TYPE_UNIX_TIMESTAMPING_MESSAGE,
                 "message-type", MESSAGE_TYPE_PACKET_ID,
                 "packet-id", errqueue->ee_data,
                 NULL);
      printf("** TUNK ** GOT IP_RECVERR! PacketID:%u\n", errqueue->ee_data);
      return data;
    }
  }

  if (level == SOL_SOCKET && type == SCM_TIMESTAMPING)
  {
    struct timespec* udp_tx_stamp = (struct timespec*) data;

    if (size != G_UNIX_TIMESTAMPING_RECV_SIZE || data == NULL)
    {
      g_warning ("Expected a timestamping struct of %" G_GSIZE_FORMAT " bytes but " "got %" G_GSIZE_FORMAT " bytes of data", G_UNIX_TIMESTAMPING_RECV_SIZE, size);
      return NULL;
    } else {
      guint timestamping_type = __TIMESTAMPING_TYPE_MAX__;
      if (udp_tx_stamp[TIMESTAMPING_TYPE_RAW].tv_sec != 0 || udp_tx_stamp[TIMESTAMPING_TYPE_RAW].tv_nsec != 0 ) {
        timestamping_type = TIMESTAMPING_TYPE_RAW;
      } else if (udp_tx_stamp[TIMESTAMPING_TYPE_SYSTEM].tv_sec != 0 || udp_tx_stamp[TIMESTAMPING_TYPE_SYSTEM].tv_nsec != 0 ){
        timestamping_type = TIMESTAMPING_TYPE_SYSTEM;
      } else if (udp_tx_stamp[TIMESTAMPING_TYPE_TRANSFORMED].tv_sec != 0 || udp_tx_stamp[TIMESTAMPING_TYPE_TRANSFORMED].tv_nsec != 0 ) {
        timestamping_type = TIMESTAMPING_TYPE_TRANSFORMED;
      } else {
        g_warning ("Received timestamping information with empty timestamping info, ignoring.");
        return NULL;
      }

      if (timestamping_type != __TIMESTAMPING_TYPE_MAX__){
        GSocketControlMessage * data = g_object_new (G_TYPE_UNIX_TIMESTAMPING_MESSAGE,
                   "message-type", MESSAGE_TYPE_TIMESTAMPING_INFO,
                   "timestamping-type", timestamping_type,
                   "timestamping-secs", udp_tx_stamp[timestamping_type].tv_sec,
                   "timestamping-nsecs", udp_tx_stamp[timestamping_type].tv_nsec,
                   NULL);
        printf("** TUNK ** GOT SCM_TIMESTAMPING! Timer:%s timestamp:%ld.%09ld\n", TIMESTAMPING_TYPE_TO_STRING[timestamping_type], udp_tx_stamp[timestamping_type].tv_sec, udp_tx_stamp[timestamping_type].tv_nsec);
        return data;        
      }
    }
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
    case PROP_MESSAGE_TYPE:
        g_value_set_uint (value, message->priv->message_type);
        break;
    case PROP_PACKET_ID:
        g_value_set_uint (value, message->priv->packet_id);
        break;
    case PROP_TIMESTAMPING_TYPE:
        g_value_set_uint (value, message->priv->timestamping_type);
        break;        
    case PROP_SECONDS:
        g_value_set_int64 (value, message->priv->timestamping_sec);
        break;
    case PROP_NANOSECONDS:
        g_value_set_int64 (value, message->priv->timestamping_nsec);
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

/*
  GObjectClass *object_class = G_OBJECT_GET_CLASS(object);
  GUnixTimestampingMessageClass *scm_class = G_UNIX_TIMESTAMPING_MESSAGE_CLASS (object_class);

  printf ("** TUNK ** enter %s, object:%p prop_id:%d\n", __FUNCTION__, object, prop_id);
  printf("CLASS: %s TYPE: %ld Expected: %d\n", G_OBJECT_CLASS_NAME(object_class), G_OBJECT_CLASS_TYPE(object_class), G_IS_UNIX_TIMESTAMPING_MESSAGE_CLASS(object_class));
  printf("VAL: %lu\n", scm_class->tunkptr++);
*/
  switch (prop_id)
    {
    case PROP_MESSAGE_TYPE:
      message->priv->message_type = g_value_get_uint(value);
      printf ("** TUNK ** prop_id:%d:PROP_MESSAGE_TYPE Value:%d\n", prop_id, message->priv->message_type);
      break;
    case PROP_PACKET_ID:
      message->priv->packet_id = g_value_get_uint(value);
      printf ("** TUNK ** prop_id:%d:PROP_PACKET_ID Value:%u\n", prop_id, message->priv->packet_id);
      break;
    case PROP_TIMESTAMPING_TYPE:
      message->priv->timestamping_type = g_value_get_uint(value);
      printf ("** TUNK ** prop_id:%d:PROP_TIMESTAMPING_TYPE Value:%ld\n", prop_id, message->priv->timestamping_sec);
      break;
    case PROP_SECONDS:
      message->priv->timestamping_sec = g_value_get_int64(value);
      printf ("** TUNK ** prop_id:%d:PROP_SECONDS Value:%ld\n", prop_id, message->priv->timestamping_sec);
      break;
    case PROP_NANOSECONDS:
      message->priv->timestamping_nsec = g_value_get_int64(value);
      printf ("** TUNK ** prop_id:%d:PROP_NANOSECONDS Value:%ld\n", prop_id, message->priv->timestamping_nsec);
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

  g_object_class_install_property (object_class, PROP_MESSAGE_TYPE,
                   g_param_spec_uint ("message-type",
							"Message_type",
							"Message_type for outgoing package timestamping info.",
                            MESSAGE_TYPE_UNSET,
                            MESSAGE_TYPE_TIMESTAMPING_INFO,
                            MESSAGE_TYPE_UNSET,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_PACKET_ID,
                   g_param_spec_uint ("packet-id",
                            "Packet_id",
              "Packet id of responding sent packet (counter per socket).",
                            0,
                            G_MAXUINT32,
                            0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_TIMESTAMPING_TYPE,
                   g_param_spec_uint ("timestamping-type",
              "Timestamping_type",
              "Timestamping_type for outgoing package timestamping info.",
                            TIMESTAMPING_TYPE_SYSTEM,
                            TIMESTAMPING_TYPE_RAW,
                            TIMESTAMPING_TYPE_SYSTEM,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_SECONDS,
                   g_param_spec_int64 ("timestamping-secs",
							"Timestamp_secs",
							"Timestamping seconds of outgoing packet.",
                            0,
                            G_MAXINT64,
                            0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT_ONLY));
  g_object_class_install_property (object_class, PROP_NANOSECONDS,
                   g_param_spec_int64 ("timestamping-nsecs",
                            "Timestamp_nsecs",
							"Timestamping nanoseconds of outgoing packet.",
                            0,
                            G_MAXINT64,
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

const gchar *
g_unix_timestamping_get_message_type_name (guint message_type)
{
  return (message_type < __MESSAGE_TYPE_MAX__) ? MESSAGE_TYPE_TO_STRING[message_type] : NULL;
}

const gchar *
g_unix_timestamping_get_timestamping_type_name (guint timestamping_type)
{
  return (timestamping_type < __TIMESTAMPING_TYPE_MAX__) ? TIMESTAMPING_TYPE_TO_STRING[timestamping_type] : NULL;
}

gint
g_unix_timestamping_enable_raw(const gchar * ifname)
{
  int sd;
  struct ifreq ifr = {0};
  struct hwtstamp_config hwc = {0};

  if (ifname == NULL ){
    errno = EINVAL;
    return -1;
  }

  sd = socket(AF_LOCAL, SOCK_STREAM, 0);
  if (sd == -1){
    return -1;
  }

  ifr.ifr_data = (char*)&hwc;
  strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
  if ((ioctl(sd, SIOCGHWTSTAMP, &ifr)) == -1){
    close(sd);
    return -1;
  }

  if (hwc.tx_type != HWTSTAMP_TX_ON){
    hwc.tx_type = HWTSTAMP_TX_ON;
    if ((ioctl(sd, SIOCSHWTSTAMP, &ifr)) == -1){
      close(sd);
      return -1;
    }
  }

  close(sd);
  return 0;
}


GUnixTimestampingMessageParsed * 
g_unix_timestamping_parse_controlmessage_set(GSocketControlMessage ** control_messages[2], guint num_control_messages)
{
  GUnixTimestampingMessageParsed * data = NULL;

  if (num_control_messages == 2){
    GValue val_packet_id = G_VALUE_INIT;
    GValue val_timestamping_type = G_VALUE_INIT;
    GValue val_secs = G_VALUE_INIT;
    GValue val_nsecs = G_VALUE_INIT;

    for (guint i=0; i < num_control_messages; i++){
      gint level = g_socket_control_message_get_level(*control_messages[i]);
      gint type = g_socket_control_message_get_msg_type(*control_messages[i]);

      if (level == SOL_SOCKET && type == SCM_TIMESTAMPING && G_IS_UNIX_TIMESTAMPING_MESSAGE(control_messages[i])){
        GValue val_message_type = G_VALUE_INIT;
        g_value_init(&val_message_type, G_TYPE_UINT);
        g_object_get_property(G_OBJECT(control_messages[i]), "message-type", &val_message_type);
        if (g_value_get_int(&val_message_type) == MESSAGE_TYPE_PACKET_ID){
          g_value_init(&val_packet_id, G_TYPE_UINT);
          g_object_get_property(G_OBJECT(control_messages[i]), "packet-id", &val_packet_id);
        } else if (g_value_get_int(&val_message_type) == MESSAGE_TYPE_TIMESTAMPING_INFO)  {
          g_value_init(&val_timestamping_type, G_TYPE_UINT);
          g_value_init(&val_secs, G_TYPE_INT64);
          g_value_init(&val_nsecs, G_TYPE_INT64);
          g_object_get_property(G_OBJECT(control_messages[i]), "timestamping-type", &val_timestamping_type);
          g_object_get_property(G_OBJECT(control_messages[i]), "timestamping-secs", &val_secs);
          g_object_get_property(G_OBJECT(control_messages[i]), "timestamping-nsecs", &val_nsecs);
        }
        g_value_unset(&val_message_type);
      }
    }

    if (G_VALUE_HOLDS(&val_packet_id, G_TYPE_UINT) && G_VALUE_HOLDS(&val_timestamping_type, G_TYPE_UINT) && G_VALUE_HOLDS(&val_secs, G_TYPE_INT64) && G_VALUE_HOLDS(&val_nsecs, G_TYPE_INT64)) {
      data = g_new(GUnixTimestampingMessageParsed, 1);
      data->packet_id = g_value_get_uint(&val_packet_id);
      data->timestamping_type = g_value_get_uint(&val_timestamping_type);
      data->timestamping_sec = g_value_get_int64(&val_secs);
      data->timestamping_nsec = g_value_get_int64(&val_nsecs);
    }    

    g_value_unset(&val_timestamping_type);
    g_value_unset(&val_packet_id);
    g_value_unset(&val_secs);
    g_value_unset(&val_nsecs);
  }

  return data;
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