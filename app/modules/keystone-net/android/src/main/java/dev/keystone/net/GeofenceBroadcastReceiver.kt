package dev.keystone.net

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import com.google.android.gms.location.Geofence
import com.google.android.gms.location.GeofencingEvent

/**
 * Receives the system's geofence transitions.
 *
 * The geofence service runs in the system, so this receiver is what wakes the
 * app when the phone crosses the zone — the app itself is killed in the
 * background long before that.
 *
 * The receiver does the smallest possible thing: work out whether the phone
 * entered or left, and hand that to the module. The module then reports it to
 * the hub.
 */
class GeofenceBroadcastReceiver : BroadcastReceiver() {
    companion object {
        private const val TAG = "KeystoneNet"
    }

    override fun onReceive(context: Context, intent: Intent) {
        val event = GeofencingEvent.fromIntent(intent)
        if (event == null) {
            Log.w(TAG, "geofence event missing")
            return
        }
        if (event.hasError()) {
            Log.w(TAG, "geofence error: ${event.errorCode}")
            return
        }
        val transition = event.geofenceTransition
        val geofence = event.triggeringGeofences?.firstOrNull() ?: return
        if (geofence.requestId != GeofenceWatcher.GEOFENCE_ID) return

        when (transition) {
            Geofence.GEOFENCE_TRANSITION_ENTER -> KeystonePresenceBridge.reportEnter()
            Geofence.GEOFENCE_TRANSITION_EXIT -> {
                // Not "away" yet: the phone may only have stepped outside for a
                // moment. The watcher confirms it after a few minutes.
                KeystonePresenceBridge.reportExit()
            }
            else -> Log.w(TAG, "geofence transition $transition")
        }
    }
}

/**
 * The bridge between the receiver (which the system instantiates) and the module
 * (which lives as long as the app). The receiver may run in a process the app
 * has left behind, so the state is kept here rather than in the module.
 */
object KeystonePresenceBridge {
    /** Set by the module when it starts; null when the app is not running. */
    var watcher: GeofenceWatcher? = null

    fun reportEnter() {
        watcher?.onEnter()
    }

    fun reportExit() {
        watcher?.onExit()
    }
}
