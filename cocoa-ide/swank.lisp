;;;; -*- Mode: Lisp; Syntax: ANSI-Common-Lisp; Base: 10; Package: cl-user -*-
;;;; ***********************************************************************
;;;; FILE IDENTIFICATION
;;;;
;;;; Name:          swank.lisp
;;;; Project:       CCL IDE
;;;; Purpose:       CCL's swank loader
;;;;
;;;; ***********************************************************************

;;; ABOUT
;;; ------------------------------------------------------------------------
;;; implements tools used to loacte and load a swank server at app startup.
;;; provides an interface through which a client program can request
;;; loading of a specific copy of swank for use with SLIME
;;;
;;; ccl == the command-line lisp executable
;;; CCL == the Cocoa lisp application
;;;
;;; CCL/ccl starts a swank server in one of the following ways:
;;;
;;; 1. Emacs starts ccl as an inferior SLIME process
;;;    In this case, emacs tells ccl at startup where to get the swank
;;;    loader. ccl loads the swank indicated by the input from emacs
;;;    and starts it up
;;;
;;; 2. Emacs connects to an already-running CCL

;;;    If CCL starts up from the Finder, not under the control of an
;;;    emacs process, it starts a swank listener. The swank listener
;;;    listens on a port for connections using the swank protocol.


(in-package :GUI)

(defparameter *default-gui-swank-port* 4564)
(defparameter *active-gui-swank-port* nil)
(defparameter *ccl-swank-active-p* nil)

(defparameter *default-swank-listener-port* 4884)
(defparameter *active-gui-swank-listener-port* nil)
(defparameter *ccl-swank-listener-active-p* nil)

(load #P"ccl:cocoa-ide;slime;swank-loader.lisp")
(swank-loader::load-swank)

;;; preference-swank-port
;;; returns the current value of the "Swank Port" user preference
(defun preference-swank-port ()
  (with-autorelease-pool
    (let* ((defaults (handler-case (#/values (#/sharedUserDefaultsController ns:ns-user-defaults-controller))
                       (serious-condition (c) 
                         (progn (log-debug "~%ERROR: Unable to get preferences from the Shared User Defaults Controller: ~A"
                                           c)
                                nil))))
           (swank-port-pref (and defaults (#/valueForKey: defaults #@"swankPort"))))
      (cond
        ;; the user default is not initialized
        ((or (null swank-port-pref)
             (%null-ptr-p swank-port-pref)) nil)
        ;; examine the user default
        ((typep swank-port-pref 'ns:ns-string) 
         (handler-case (let* ((port-str (lisp-string-from-nsstring swank-port-pref))
                              (port (parse-integer port-str :junk-allowed nil)))
                         (or port *default-gui-swank-port*))
           ;; parsing the port number failed
           (ccl::parse-integer-not-integer-string (c)
             (declare (ignore c))
             (setf *ccl-swank-active-p* nil)
             (#_NSLog #@"\nError starting swank server; the swank-port user preference is not a valid port number: %@\n"
                    :id swank-port-pref)
             nil)))
        ;; the user default value is incomprehensible
        (t (progn
             (#_NSLog #@"\nERROR: Unrecognized value type in user preference 'swankPort': %@"
                    :id swank-port-pref)
             nil))))))

;;; try-starting-swank (&key (force nil))
;;; attempts to start the swank server. If :force t is supplied,
;;; ignores the "Start Swank Server?" user preference and starts the
;;; server no matter what its value

(defun try-starting-swank (&key (force nil))
  (unless *ccl-swank-active-p*
    ;; try to determine the user preferences concerning the swank port number
    ;; and whether the swank server should be started. If the user says start
    ;; it, and we can determine a valid port for it, start it up
    (let* ((start-swank? (or force (preference-start-swank?)))
           (swank-port (or (preference-swank-port) *default-gui-swank-port*)))
      (if (and start-swank? swank-port)
          ;; try to start the swank server
          (handler-case (progn
                          (swank:create-server :port swank-port :dont-close t)
                          (setf *ccl-swank-active-p* t)
                          (setf *active-gui-swank-port* swank-port)
                          swank-port)
            ;; swank server creation failed
            (serious-condition (c)
              (setf *ccl-swank-active-p* nil)
              (setf *active-gui-swank-port* nil)
              (log-debug "~%Error starting swank server: ~A~%" c)
              nil))
          ;; don't try to start the swank server
          (progn
            (setf *ccl-swank-active-p* nil)
            (setf *active-gui-swank-port* nil)
            nil)))))


;;; start-swank-listener
;;; -----------------------------------------------------------------
;;; starts up CCL's swank-listener server on the specified port

;;; aux utils

(defvar $emacs-ccl-swank-request-marker "[emacs-ccl-swank-request]")

(defstruct (swank-status (:conc-name swank-))
  (active? nil :read-only t)
  (requested-loader nil :read-only t)
  (requested-port nil :read-only t))

(defun not-ready-yet (nm)
  (error "Not yet implemented: ~A" nm))

(defun read-swank-ping (tcp-stream) 
  (read-line tcp-stream nil nil nil))

(defun parse-swank-ping (p) 
  (let ((sentinel-end (length $emacs-ccl-swank-request-marker)))
    (if (typep p 'string)
        (if (string= p $emacs-ccl-swank-request-marker :start1 0 :end1 sentinel-end)
            (let* ((request (subseq p sentinel-end))
                   (split-pos (position #\: request))
                   (port-str (if split-pos
                                 (subseq request 0 split-pos)
                                 nil))
                   (port (when port-str (parse-integer port-str :junk-allowed nil)))
                   (path-str (if split-pos
                                 (subseq request (1+ split-pos))
                                 request)))
              (values (string-trim '(#\space #\tab #\return #\newline) path-str) port))
            nil)
        nil)))

(defun load-and-start-swank (path requested-port) 
  (handler-case (progn
                  (load path)
                  (swank:create-server :port requested-port :dont-close t)
                  (make-swank-status :active? t :requested-loader path :requested-port requested-port))
    (ccl::socket-creation-error (e) (log-debug "Unable to start a swank server on port: ~A; ~A"
                                               requested-port e)
                                (make-swank-status :active? nil :requested-loader path :requested-port requested-port))
    (serious-condition (e) (log-debug "There was a problem creating the swank server on port ~A: ~A"
                                      requested-port e)
                       (make-swank-status :active? nil :requested-loader path :requested-port requested-port))))

(defun swank-ready? (status)
  (swank-active? status))

(defun send-swank-response (tcp-stream status)
  (let ((response (format nil "(:active ~S :loader ~S :port ~D)"
                          (swank-active? status)
                          (swank-requested-loader status)
                          (swank-requested-port status))))
    (format tcp-stream response)
    (finish-output tcp-stream)))

(defun handle-swank-client (c)
  (let* ((msg (read-swank-ping c)))
    (multiple-value-bind (swank-path requested-port)
        (parse-swank-ping msg)
      (load-and-start-swank swank-path requested-port))))

;;; the real deal
;;; if it succeeds, it returns a PROCESS object
;;; if it fails, it returns a CONDITION object
(defun start-swank-listener (&optional (port *default-swank-listener-port*))
  (handler-case (with-open-socket (sock :type :stream :connect :passive :local-port port :reuse-address t :auto-close t)
                  (loop
                     (format t "~%swank listener loop...")
                     (force-output)
                     (let* ((client-sock (accept-connection sock))
                            (status (handle-swank-client client-sock)))
                       (send-swank-response client-sock status))))
    (ccl::socket-creation-error (c) (nslog-condition c "Unable to create a socket for the swank-listener: ") c)
    (ccl::socket-error (c) (nslog-condition c "Swank-listener failed trying to accept a client conection: ") c)
    (serious-condition (c) (nslog-condition c "Error in the swank-listener:") c)))

;;; maybe-start-swank-listener
;;; -----------------------------------------------------------------
;;; checks whether to start the ccl swank listener, and starts it if
;;; warranted.
(defun maybe-start-swank-listener (&optional (force nil))
  (unless *ccl-swank-active-p*
    ;; try to determine the user preferences concerning the swank port number
    ;; and whether the swank listener should be started. If the user says start
    ;; it, and we can determine a valid port for it, start it up
    (let* ((start-swank-listener? (or (preference-start-swank?) force))
           (swank-listener-port (or (preference-swank-port) *default-gui-swank-port*)))
      (if (and start-swank-listener? swank-listener-port)
          ;; try to start the swank listener
          (handler-case (let ((swank-listener (start-swank-listener swank-listener-port)))
                          (if (typep swank-listener 'process)
                              (progn
                                (setf *active-gui-swank-listener-port* swank-listener-port)
                                (setf *ccl-swank-listener-active-p* t)
                                swank-listener-port)
                              (progn
                                (setf *active-gui-swank-listener-port* nil)
                                (setf *ccl-swank-listener-active-p* nil)
                                nil)))
            ;; swank listener creation failed
            (serious-condition (c)
              (setf *active-gui-swank-listener-port* nil)
              (setf *ccl-swank-listener-active-p* nil)
              (log-debug "~%Error starting swank server: ~A~%" c)
              nil))
          ;; don't try to start the swank listener
          (progn
            (setf *active-gui-swank-listener-port* nil)
            (setf *ccl-swank-listener-active-p* nil)
            nil)))))

(provide :swank)