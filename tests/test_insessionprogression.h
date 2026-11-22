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
    void advanceCramPromotesAndResetsTheCurrentLeitnerBox();
    void cramIntervalsCountOtherCards();
    void cramQueueNeverEndsForOneCard();
    void emptyCramHasNoCurrentLeitnerBox();
    void removeDropsReviewAndCramCards();
    void clearDropsRepeatsAndLeitnerBox();
    void switchingToCramDropsPendingRepeats();
};
