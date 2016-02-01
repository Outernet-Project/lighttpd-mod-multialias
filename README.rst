mod_multialias
==============

A forked version of the *mod_alias* lighttpd plugin which allows specifying multiple source directories for a given path::

    alias.url = ( "/static/" => "/path/to/static/root",
                  "/media/"  => ("/path/to/first/folder",
                                 "/path/to/second/folder") )

Given such a configuration, when the ``/media/`` path is matched, an existence check will be performed first on the file system for the requested resource. In case the first folder in the specified list does not contain the resource, the next folder will be checked for it, etc. This does involve a performance penalty, however such checks will only be enabled if the *alias* configuration contains arrays of folders. In case there aren't any, it should behave exactly as the regular *mod_alias* plugin.

