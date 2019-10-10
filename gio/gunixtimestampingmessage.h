/* GIO - GLib Input, Output and Streaming Library - socket timestamping.
 *
 * Copyright (C) 2019 Pexip AS
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Knut Saastad <knut@pexip.com>
 */

#ifndef __G_UNIX_TIMESTAMPING_MESSAGE_H__
#define __G_UNIX_TIMESTAMPING_MESSAGE_H__

#include <gio/gio.h>

G_BEGIN_DECLS
#define G_TYPE_UNIX_TIMESTAMPING_MESSAGE                              (g_unix_timestamping_message_get_type ())
#define G_UNIX_TIMESTAMPING_MESSAGE(inst)                             (G_TYPE_CHECK_INSTANCE_CAST ((inst),                     \
                                                             G_TYPE_UNIX_TIMESTAMPING_MESSAGE, GUnixTimestampingMessage))
#define G_UNIX_TIMESTAMPING_MESSAGE_CLASS(class)                      (G_TYPE_CHECK_CLASS_CAST ((class),                       \
                                                             G_TYPE_UNIX_TIMESTAMPING_MESSAGE, GUnixTimestampingMessageClass))
#define G_IS_UNIX_TIMESTAMPING_MESSAGE(inst)                          (G_TYPE_CHECK_INSTANCE_TYPE ((inst),                     \
                                                             G_TYPE_UNIX_TIMESTAMPING_MESSAGE))
#define G_IS_UNIX_TIMESTAMPING_MESSAGE_CLASS(class)                   (G_TYPE_CHECK_CLASS_TYPE ((class),                       \
                                                             G_TYPE_UNIX_TIMESTAMPING_MESSAGE))
#define G_UNIX_TIMESTAMPING_MESSAGE_GET_CLASS(inst)                   (G_TYPE_INSTANCE_GET_CLASS ((inst),                      \
                                                             G_TYPE_UNIX_TIMESTAMPING_MESSAGE, GUnixTimestampingMessageClass))
#define G_UNIX_TIMESTAMPING_SEND_SIZE (sizeof (int))
#define G_UNIX_TIMESTAMPING_RECV_SIZE ((gsize)48)

typedef struct _GUnixTimestampingMessagePrivate GUnixTimestampingMessagePrivate;
typedef struct _GUnixTimestampingMessageClass GUnixTimestampingMessageClass;
typedef struct _GUnixTimestampingMessage GUnixTimestampingMessage;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GUnixTimestampingMessage, g_object_unref)
     struct _GUnixTimestampingMessageClass
     {
       GSocketControlMessageClass parent_class;

       /*< private > */

       /* Padding for future expansion */
       void (*_g_reserved1) (void);
       void (*_g_reserved2) (void);
     };

     struct _GUnixTimestampingMessage
     {
       GSocketControlMessage parent_instance;
       GUnixTimestampingMessagePrivate *priv;
     };

GLIB_AVAILABLE_IN_ALL GType g_unix_timestamping_message_get_type (void) G_GNUC_CONST;

GLIB_AVAILABLE_IN_ALL gboolean g_unix_timestamping_message_is_supported (void);

GLIB_AVAILABLE_IN_ALL GSocketControlMessage * g_unix_timestamping_message_new (void);

G_END_DECLS
#endif // __G_UNIX_TIMESTAMPING_MESSAGE_H__
