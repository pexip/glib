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
  MESSAGE_TYPE_UNIFIED,
  __MESSAGE_TYPE_MAX__
};

char * MESSAGE_TYPE_TO_STRING[__MESSAGE_TYPE_MAX__] = {"UNSET", "PACKET_ID", "TIMESTAMPING_INFO", "UNIFIED"};

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

G_DEFINE_TYPE_WITH_PRIVATE (GUnixTimestampingMessage, g_unix_timestamping_message, G_TYPE_SOCKET_CONTROL_MESSAGE)

static gsize g_unix_timestamping_message_get_size (GSocketControlMessage * message)
{
  return G_UNIX_TIMESTAMPING_SEND_SIZE;
}

static int
g_unix_timestamping_message_get_level (GSocketControlMessage * message)
{
  return SOL_SOCKET;
}

static int
g_unix_timestamping_message_get_msg_type (GSocketControlMessage * message)
{
  return SCM_TIMESTAMPING;
}

static GSocketControlMessage *
g_unix_timestamping_message_deserialize (int level, int type, gsize size, gpointer data)
{
  if ((level == SOL_IP || level == SOL_IPV6) && type == IP_RECVERR)
  {
    struct sock_extended_err * errqueue = (struct sock_extended_err *)data;
    if (errqueue->ee_errno == ENOMSG && errqueue->ee_origin == SO_EE_ORIGIN_TIMESTAMPING) {
      GSocketControlMessage * data = g_object_new (G_TYPE_UNIX_TIMESTAMPING_MESSAGE,
                 "message-type", MESSAGE_TYPE_PACKET_ID,
                 "packet-id", errqueue->ee_data,
                 NULL);
      g_debug("Got control message type IP_RECVERR with origin SO_EE_ORIGIN_TIMESTAMPING! PacketID:%u\n", errqueue->ee_data);
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
        g_debug("Got control message type SCM_TIMESTAMPING! Timer:%s timestamp:%ld.%09ld\n", TIMESTAMPING_TYPE_TO_STRING[timestamping_type], udp_tx_stamp[timestamping_type].tv_sec, udp_tx_stamp[timestamping_type].tv_nsec);
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
  *data = SOF_TIMESTAMPING_TX_HARDWARE | SOF_TIMESTAMPING_TX_SOFTWARE;
}

static void
g_unix_timestamping_message_finalize (GObject *object)
{
  GUnixTimestampingMessage *message = G_UNIX_TIMESTAMPING_MESSAGE (object);

  G_OBJECT_CLASS (g_unix_timestamping_message_parent_class) ->finalize (object);
}

static void
g_unix_timestamping_message_get_property (GObject    *object,
                                         guint       prop_id,
                                         GValue     *value,
                                         GParamSpec *pspec)
{
  GUnixTimestampingMessage *message = G_UNIX_TIMESTAMPING_MESSAGE (object);
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
}


static void
g_unix_timestamping_message_set_property (GObject      *object,
                                         guint         prop_id,
                                         const GValue *value,
                                         GParamSpec   *pspec)
{
  GUnixTimestampingMessage *message = G_UNIX_TIMESTAMPING_MESSAGE (object);

  switch (prop_id)
    {
    case PROP_MESSAGE_TYPE:
      message->priv->message_type = g_value_get_uint(value);
      break;
    case PROP_PACKET_ID:
      message->priv->packet_id = g_value_get_uint(value);
      break;
    case PROP_TIMESTAMPING_TYPE:
      message->priv->timestamping_type = g_value_get_uint(value);
      break;
    case PROP_SECONDS:
      message->priv->timestamping_sec = g_value_get_int64(value);
      break;
    case PROP_NANOSECONDS:
      message->priv->timestamping_nsec = g_value_get_int64(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_unix_timestamping_message_init (GUnixTimestampingMessage * message)
{
  message->priv = g_unix_timestamping_message_get_instance_private(message);
}

static void
g_unix_timestamping_message_class_init (GUnixTimestampingMessageClass * class)
{
  GSocketControlMessageClass *scm_class = G_SOCKET_CONTROL_MESSAGE_CLASS (class);
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  scm_class->get_size = g_unix_timestamping_message_get_size;
  scm_class->get_level = g_unix_timestamping_message_get_level;
  scm_class->get_type = g_unix_timestamping_message_get_msg_type;
  scm_class->serialize = g_unix_timestamping_message_serialize;
  scm_class->deserialize = g_unix_timestamping_message_deserialize;
  object_class->finalize = g_unix_timestamping_message_finalize;
  object_class->set_property = g_unix_timestamping_message_set_property;
  object_class->get_property = g_unix_timestamping_message_get_property;

  g_object_class_install_property (object_class, PROP_MESSAGE_TYPE,
                   g_param_spec_uint ("message-type",
							"Message_type",
							"Message_type for outgoing package timestamping info.",
                            MESSAGE_TYPE_UNSET,
                            MESSAGE_TYPE_UNIFIED,
                            MESSAGE_TYPE_UNSET,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE |
                            G_PARAM_CONSTRUCT));
  g_object_class_install_property (object_class, PROP_PACKET_ID,
                   g_param_spec_uint ("packet-id",
                            "Packet_id",
              "Packet id of responding sent packet (per socket counter, starting at 0).",
                            0,
                            G_MAXUINT32,
                            0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE));
  g_object_class_install_property (object_class, PROP_TIMESTAMPING_TYPE,
                   g_param_spec_uint ("timestamping-type",
              "Timestamping_type",
              "Timestamping_type for outgoing package timestamping info.",
                            TIMESTAMPING_TYPE_SYSTEM,
                            TIMESTAMPING_TYPE_RAW,
                            TIMESTAMPING_TYPE_SYSTEM,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE));
  g_object_class_install_property (object_class, PROP_SECONDS,
                   g_param_spec_int64 ("timestamping-secs",
							"Timestamp_secs",
							"Timestamping seconds of outgoing packet.",
                            0,
                            G_MAXINT64,
                            0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE));
  g_object_class_install_property (object_class, PROP_NANOSECONDS,
                   g_param_spec_int64 ("timestamping-nsecs",
                            "Timestamp_nsecs",
							"Timestamping nanoseconds of outgoing packet.",
                            0,
                            G_MAXINT64,
                            0,
                            G_PARAM_STATIC_STRINGS |
                            G_PARAM_READWRITE));
}

gboolean
g_unix_timestamping_message_is_supported (void)
{
  return TRUE;
}

const gchar *
g_unix_timestamping_get_message_type_name (guint message_type)
{
  return (message_type < __MESSAGE_TYPE_MAX__) ? MESSAGE_TYPE_TO_STRING[message_type] : "INVALID";
}

const gchar *
g_unix_timestamping_get_timestamping_type_name (guint timestamping_type)
{
  return (timestamping_type < __TIMESTAMPING_TYPE_MAX__) ? TIMESTAMPING_TYPE_TO_STRING[timestamping_type] : "INVALID";
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


GUnixTimestampingMessageUnified * 
g_unix_timestamping_unify_control_message_set(GSocketControlMessage * control_messages[2])
{
  GUnixTimestampingMessageUnified * unified_message = NULL;

  GValue val_packet_id = G_VALUE_INIT;
  GValue val_timestamping_type = G_VALUE_INIT;
  GValue val_secs = G_VALUE_INIT;
  GValue val_nsecs = G_VALUE_INIT;

  for (guint i=0; i < 2; i++){
    GSocketControlMessage * control_message = control_messages[i];
    gint level = g_socket_control_message_get_level(control_message);
    gint type = g_socket_control_message_get_msg_type(control_message);

    if (level != SOL_SOCKET){
      g_warning("Expected GSocketControlMessage with level SOL_SOCKET(%u), got %u!", SOL_SOCKET, level);
    } else if (type != SCM_TIMESTAMPING){
      g_warning("Expected GSocketControlMessage with msg_type SCM_TIMESTAMPING(%u), got %u!", SCM_TIMESTAMPING, type);
    } else if (G_IS_UNIX_TIMESTAMPING_MESSAGE(control_message) == FALSE){
      gpointer obj_class_id = G_OBJECT_GET_CLASS(control_message);
      const gchar * obj_class_name = "object_class_unknown";
      if (obj_class_id != NULL){
        obj_class_name = G_OBJECT_CLASS_NAME(obj_class_id);        
      }
      g_warning("Expected object_class UNIX_TIMESTAMPING_MESSAGE, got %s!", obj_class_name);
    } else {
      GValue val_message_type = G_VALUE_INIT;
      g_value_init(&val_message_type, G_TYPE_UINT);
      g_object_get_property(G_OBJECT(control_message), "message-type", &val_message_type);
      if (g_value_get_uint(&val_message_type) == MESSAGE_TYPE_PACKET_ID){
        g_value_init(&val_packet_id, G_TYPE_UINT);
        g_object_get_property(G_OBJECT(control_message), "packet-id", &val_packet_id);
        g_debug("Got message-type:%s, packet-id:%u", 
                g_unix_timestamping_get_message_type_name(g_value_get_uint(&val_message_type)),
                g_value_get_uint(&val_packet_id));
      } else if (g_value_get_uint(&val_message_type) == MESSAGE_TYPE_TIMESTAMPING_INFO)  {
        g_value_init(&val_timestamping_type, G_TYPE_UINT);
        g_value_init(&val_secs, G_TYPE_INT64);
        g_value_init(&val_nsecs, G_TYPE_INT64);
        g_object_get_property(G_OBJECT(control_message), "timestamping-type", &val_timestamping_type);
        g_object_get_property(G_OBJECT(control_message), "timestamping-secs", &val_secs);
        g_object_get_property(G_OBJECT(control_message), "timestamping-nsecs", &val_nsecs);
        g_debug("Got message-type:%s, timestamping-type:%u timestamping-type-name:%s timestamping-secs:%lu timestamping-nsecs:%lu", 
                g_unix_timestamping_get_message_type_name(g_value_get_uint(&val_message_type)), 
                g_value_get_uint(&val_timestamping_type),
                g_unix_timestamping_get_timestamping_type_name(g_value_get_uint(&val_timestamping_type)),
                g_value_get_int64(&val_secs),
                g_value_get_int64(&val_nsecs));
      }
      g_value_unset(&val_message_type);
    }
  }

  if (G_VALUE_HOLDS(&val_packet_id, G_TYPE_UINT) && G_VALUE_HOLDS(&val_timestamping_type, G_TYPE_UINT) && G_VALUE_HOLDS(&val_secs, G_TYPE_INT64) && G_VALUE_HOLDS(&val_nsecs, G_TYPE_INT64)) {
    unified_message = g_new (GUnixTimestampingMessageUnified, 1);
    unified_message->packet_id = g_value_get_uint(&val_packet_id);
    unified_message->timestamping_type = g_value_get_uint(&val_timestamping_type);
    unified_message->timestamping_sec = g_value_get_int64(&val_secs);
    unified_message->timestamping_nsec = g_value_get_int64(&val_nsecs);
  }    

  g_value_unset(&val_timestamping_type);
  g_value_unset(&val_packet_id);
  g_value_unset(&val_secs);
  g_value_unset(&val_nsecs);


  return unified_message;
}

gint 
g_unix_timestamping_unify_control_message_set_inplace(GSocketControlMessage * control_messages[2])
{
  gint err = -1;
  GSocketControlMessage * unified_control_message = NULL;

  GValue val_packet_id = G_VALUE_INIT;
  GValue val_timestamping_type = G_VALUE_INIT;
  GValue val_secs = G_VALUE_INIT;
  GValue val_nsecs = G_VALUE_INIT;

  for (guint i=0; i < 2; i++){
    GSocketControlMessage * control_message = control_messages[i];
    gint level = g_socket_control_message_get_level(control_message);
    gint type = g_socket_control_message_get_msg_type(control_message);

    if (level != SOL_SOCKET){
      g_warning("Expected GSocketControlMessage with level SOL_SOCKET(%u), got %u!", SOL_SOCKET, level);
    } else if (type != SCM_TIMESTAMPING){
      g_warning("Expected GSocketControlMessage with msg_type SCM_TIMESTAMPING(%u), got %u!", SCM_TIMESTAMPING, type);
    } else if (G_IS_UNIX_TIMESTAMPING_MESSAGE(control_message) == FALSE){
      gpointer obj_class_id = G_OBJECT_GET_CLASS(control_message);
      const gchar * obj_class_name = "object_class_unknown";
      if (obj_class_id != NULL){
        obj_class_name = G_OBJECT_CLASS_NAME(obj_class_id);        
      }
      g_warning("Expected object_class UNIX_TIMESTAMPING_MESSAGE, got %s!", obj_class_name);
    } else {
      GValue val_message_type = G_VALUE_INIT;
      g_value_init(&val_message_type, G_TYPE_UINT);
      g_object_get_property(G_OBJECT(control_message), "message-type", &val_message_type);
      if (g_value_get_uint(&val_message_type) == MESSAGE_TYPE_PACKET_ID){
        g_value_init(&val_packet_id, G_TYPE_UINT);
        g_object_get_property(G_OBJECT(control_message), "packet-id", &val_packet_id);
        g_debug("Got message-type:%s, packet-id:%u", 
                g_unix_timestamping_get_message_type_name(g_value_get_uint(&val_message_type)),
                g_value_get_uint(&val_packet_id));
      } else if (g_value_get_uint(&val_message_type) == MESSAGE_TYPE_TIMESTAMPING_INFO)  {
        g_value_init(&val_timestamping_type, G_TYPE_UINT);
        g_value_init(&val_secs, G_TYPE_INT64);
        g_value_init(&val_nsecs, G_TYPE_INT64);
        g_object_get_property(G_OBJECT(control_message), "timestamping-type", &val_timestamping_type);
        g_object_get_property(G_OBJECT(control_message), "timestamping-secs", &val_secs);
        g_object_get_property(G_OBJECT(control_message), "timestamping-nsecs", &val_nsecs);
        g_debug("Got message-type:%s, timestamping-type:%u timestamping-type-name:%s timestamping-secs:%lu timestamping-nsecs:%lu", 
                g_unix_timestamping_get_message_type_name(g_value_get_uint(&val_message_type)), 
                g_value_get_uint(&val_timestamping_type),
                g_unix_timestamping_get_timestamping_type_name(g_value_get_uint(&val_timestamping_type)),
                g_value_get_int64(&val_secs),
                g_value_get_int64(&val_nsecs));
      }
      g_value_unset(&val_message_type);
    }
  }

  if (G_VALUE_HOLDS(&val_packet_id, G_TYPE_UINT) && G_VALUE_HOLDS(&val_timestamping_type, G_TYPE_UINT) && G_VALUE_HOLDS(&val_secs, G_TYPE_INT64) && G_VALUE_HOLDS(&val_nsecs, G_TYPE_INT64)) {
    GValue val_message_type = G_VALUE_INIT;
    g_value_init(&val_message_type, G_TYPE_UINT);
    g_value_set_uint(&val_message_type, MESSAGE_TYPE_UNIFIED);

    for (guint i=0; i < 2; i++){
      GSocketControlMessage * control_message = control_messages[i];      

      g_object_set_property(control_message, "message-type", &val_message_type);
      g_object_set_property(control_message, "packet-id", &val_packet_id);
      g_object_set_property(control_message, "timestamping-type", &val_timestamping_type);
      g_object_set_property(control_message, "timestamping-secs", &val_secs);
      g_object_set_property(control_message, "timestamping-nsecs", &val_nsecs);
    }

    g_value_unset(&val_message_type);
    err = 0;
  }    

  g_value_unset(&val_timestamping_type);
  g_value_unset(&val_packet_id);
  g_value_unset(&val_secs);
  g_value_unset(&val_nsecs);

  return err;
}

GSocketControlMessage *
g_unix_timestamping_message_new (void)
{
  GUnixTimestampingMessage * msg = NULL;
  msg =  g_object_new (G_TYPE_UNIX_TIMESTAMPING_MESSAGE, NULL);
  return msg;
}
