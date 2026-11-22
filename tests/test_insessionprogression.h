#pragma once

#include <QObject>

class TestInSessionProgression : public QObject {
    Q_OBJECT

private slots:
    void nextResultCarriesKindCardWaitAndLeitnerBox();
    void reviewOrderComesFromNextResults();
    void upcomingPeeksWithoutConsuming();
    void pendingRepeatWaitComesFromNextResult();
    void nextResultUsesSmallestPendingDeadline();
    void rebuildPreservesAbsolutePendingDeadlines();
    void pendingAlreadyDueIsNotDuplicated();
    void repeatAfterOverwritesDeadline();
    void pendingRepeatsIsACopy();
    void buildCramKeepsTheGivenOrder();
    void advanceCramPromotesAndResetsTheCurrentBox();
    void cramIntervalsCountOtherCards();
    void cramQueueNeverEndsForOneCard();
    void emptyCramHasNoCurrentBox();
    void removeDropsReviewAndCramCards();
    void clearDropsRepeatsAndLeitnerBox();
    void switchingToCramDropsPendingRepeats();
};
