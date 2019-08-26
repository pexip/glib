#ifndef __G_SOCKET_TIMESTAMPING_MESSAGE_H__
#define __G_SOCKET_TIMESTAMPING_MESSAGE_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define G_TYPE_SOCKET_TIMESTAMPING_MESSAGE                              (g_socket_timestamping_message_get_type ())
#define G_SOCKET_TIMESTAMPING_MESSAGE(inst)                             (G_TYPE_CHECK_INSTANCE_CAST ((inst),                     \
                                                             G_TYPE_SOCKET_TIMESTAMPING_MESSAGE, GSocketTimestampingMessage))
#define G_SOCKET_TIMESTAMPING_MESSAGE_CLASS(class)                      (G_TYPE_CHECK_CLASS_CAST ((class),                       \
                                                             G_TYPE_SOCKET_TIMESTAMPING_MESSAGE, GSocketTimestampingMessageClass))
#define G_IS_SOCKET_TIMESTAMPING_MESSAGE(inst)                          (G_TYPE_CHECK_INSTANCE_TYPE ((inst),                     \
                                                             G_TYPE_SOCKET_TIMESTAMPING_MESSAGE))
#define G_IS_SOCKET_TIMESTAMPING_MESSAGE_CLASS(class)                   (G_TYPE_CHECK_CLASS_TYPE ((class),                       \
                                                             G_TYPE_SOCKET_TIMESTAMPING_MESSAGE))
#define G_SOCKET_TIMESTAMPING_MESSAGE_GET_CLASS(inst)                   (G_TYPE_INSTANCE_GET_CLASS ((inst),                      \
                                                             G_TYPE_SOCKET_TIMESTAMPING_MESSAGE, GSocketTimestampingMessageClass))

typedef struct _GSocketTimestampingMessagePrivate                       GSocketTimestampingMessagePrivate;
typedef struct _GSocketTimestampingMessageClass                         GSocketTimestampingMessageClass;
typedef struct _GSocketTimestampingMessage                              GSocketTimestampingMessage;

G_DEFINE_AUTOPTR_CLEANUP_FUNC(GSocketTimestampingMessage, g_object_unref)

struct _GSocketTimestampingMessageClass
{
  GSocketControlMessageClass parent_class;

  /*< private >*/

  /* Padding for future expansion */
  void (*_g_reserved1) (void);
  void (*_g_reserved2) (void);
};

struct _GSocketTimestampingMessage
{
  GSocketControlMessage parent_instance;
  GSocketTimestampingMessagePrivate *priv;
};

GLIB_AVAILABLE_IN_ALL
GType                   g_socket_timestamping_message_get_type                      (void) G_GNUC_CONST;

G_END_DECLS

#endif // __G_SOCKET_TIMESTAMPING_MESSAGE_H__
