package io.github.someoneisworking.lucent;

import java.util.ArrayList;
import java.util.List;

public final class LucentTouchContactsTest {
  private static final class Received {
    final int pointerId;
    final float x;
    final float y;
    final LucentTouchContacts.Phase phase;

    Received(LucentTouchContacts.Contact contact) {
      pointerId = contact.pointerId;
      x = contact.x;
      y = contact.y;
      phase = contact.phase;
    }
  }

  private static final class Recorder implements LucentTouchContacts.Listener {
    final List<Received> contacts = new ArrayList<>();

    @Override
    public void onContact(LucentTouchContacts.Contact contact) {
      contacts.add(new Received(contact));
    }
  }

  private static void check(boolean condition) {
    if (!condition) {
      throw new AssertionError();
    }
  }

  private static void checkEvent(Received event, int pointerId, float x, float y,
                                 LucentTouchContacts.Phase phase) {
    check(event.pointerId == pointerId);
    check(event.x == x);
    check(event.y == y);
    check(event.phase == phase);
  }

  private static void preservesContactIdentityAndCancelsAllCaptures() {
    LucentTouchContacts contacts = new LucentTouchContacts();
    Recorder recorder = new Recorder();
    contacts.setListener(recorder);

    contacts.down(4, 12.0F, 18.0F, 0.5F);
    contacts.down(9, 50.0F, 80.0F, 1.0F);
    contacts.update(4, 14.0F, 20.0F, 0.7F);
    contacts.cancelAll();

    check(recorder.contacts.size() == 5);
    checkEvent(recorder.contacts.get(0), 4, 12.0F, 18.0F, LucentTouchContacts.Phase.Down);
    checkEvent(recorder.contacts.get(1), 9, 50.0F, 80.0F, LucentTouchContacts.Phase.Down);
    checkEvent(recorder.contacts.get(2), 4, 14.0F, 20.0F, LucentTouchContacts.Phase.Move);
    checkEvent(recorder.contacts.get(3), 4, 14.0F, 20.0F, LucentTouchContacts.Phase.Cancel);
    checkEvent(recorder.contacts.get(4), 9, 50.0F, 80.0F, LucentTouchContacts.Phase.Cancel);

    contacts.update(4, 17.0F, 22.0F, 1.0F);
    contacts.up(9, 50.0F, 80.0F, 1.0F);
    check(recorder.contacts.size() == 5);
  }

  private static void listenerReplacementReleasesThePriorOwner() {
    LucentTouchContacts contacts = new LucentTouchContacts();
    Recorder previous = new Recorder();
    Recorder next = new Recorder();
    contacts.setListener(previous);
    contacts.down(3, 4.0F, 5.0F, 1.0F);
    contacts.setListener(next);
    contacts.down(3, 6.0F, 7.0F, 1.0F);
    contacts.up(3, 6.0F, 7.0F, 1.0F);

    check(previous.contacts.size() == 2);
    checkEvent(previous.contacts.get(1), 3, 4.0F, 5.0F, LucentTouchContacts.Phase.Cancel);
    check(next.contacts.size() == 2);
    checkEvent(next.contacts.get(0), 3, 6.0F, 7.0F, LucentTouchContacts.Phase.Down);
    checkEvent(next.contacts.get(1), 3, 6.0F, 7.0F, LucentTouchContacts.Phase.Up);
  }

  public static void main(String[] args) {
    preservesContactIdentityAndCancelsAllCaptures();
    listenerReplacementReleasesThePriorOwner();
    System.out.println("lucent Android touch contacts: 2/2 tests passed");
  }
}
