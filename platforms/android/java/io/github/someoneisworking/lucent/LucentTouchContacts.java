package io.github.someoneisworking.lucent;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;

/**
 * Title-neutral lifetime tracking for contacts acquired by an Android SDL Activity.
 *
 * <p>The Activity translates {@code MotionEvent}s into these calls, while a title maps the
 * resulting contacts into its own controls. The listener receives immutable snapshots only for the
 * duration of the callback, so a title cannot retain Android's recycled {@code MotionEvent}.</p>
 */
public final class LucentTouchContacts {
  public enum Phase {
    Down,
    Move,
    Up,
    Cancel,
  }

  public static final class Contact {
    public final int pointerId;
    public final float x;
    public final float y;
    public final float pressure;
    public final Phase phase;

    private Contact(int pointerId, float x, float y, float pressure, Phase phase) {
      this.pointerId = pointerId;
      this.x = x;
      this.y = y;
      this.pressure = pressure;
      this.phase = phase;
    }
  }

  public interface Listener {
    void onContact(Contact contact);
  }

  private final Map<Integer, Contact> activeContacts = new LinkedHashMap<>();
  private Listener listener;

  /**
   * Replacing or clearing a listener cancels its captured contacts before the new listener runs.
   */
  public void setListener(Listener listener) {
    if (this.listener != listener) {
      cancelAll();
    }
    this.listener = listener;
  }

  public void down(int pointerId, float x, float y, float pressure) {
    if (activeContacts.containsKey(pointerId)) {
      update(pointerId, x, y, pressure);
      return;
    }
    publish(pointerId, x, y, pressure, Phase.Down, true);
  }

  public void update(int pointerId, float x, float y, float pressure) {
    if (!activeContacts.containsKey(pointerId)) {
      return;
    }
    publish(pointerId, x, y, pressure, Phase.Move, true);
  }

  public void up(int pointerId, float x, float y, float pressure) {
    if (!activeContacts.containsKey(pointerId)) {
      return;
    }
    publish(pointerId, x, y, pressure, Phase.Up, false);
  }

  /** Delivers one cancellation for every active contact, then forgets every capture. */
  public void cancelAll() {
    for (Contact contact : new ArrayList<>(activeContacts.values())) {
      publish(contact.pointerId, contact.x, contact.y, contact.pressure, Phase.Cancel, false);
    }
  }

  private void publish(int pointerId, float x, float y, float pressure, Phase phase,
                       boolean remainsActive) {
    Contact contact = new Contact(pointerId, x, y, pressure, phase);
    if (remainsActive) {
      activeContacts.put(pointerId, contact);
    } else {
      activeContacts.remove(pointerId);
    }
    if (listener != null) {
      listener.onContact(contact);
    }
  }
}
